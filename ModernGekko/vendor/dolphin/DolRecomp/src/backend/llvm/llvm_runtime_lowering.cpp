#include "backend/llvm/llvm_function_emitter.h"
#include "cpu/cpu.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

namespace dolllvm {

using namespace llvm;

namespace {

Value *emitPairedFMA(IRBuilder<> &builder, Module &module, Value *a, Value *c,
                     Value *b) {
  FunctionCallee fma = Intrinsic::getOrInsertDeclaration(
      &module, Intrinsic::fma, {Type::getDoubleTy(module.getContext())});
  return builder.CreateCall(fma, {a, c, b});
}

Value *roundPairedC(IRBuilder<> &builder, Value *bits) {
  return builder.CreateAdd(
      builder.CreateAnd(bits, builder.getInt64(0xFFFFFFFFF8000000ull)),
      builder.CreateAnd(bits, builder.getInt64(0x0000000008000000ull)));
}

Value *pairedCUnusual(IRBuilder<> &builder, Value *bits) {
  const auto expMask = builder.getInt64(0x7FF0000000000000ull);
  return builder.CreateAnd(
      builder.CreateICmpEQ(builder.CreateAnd(bits, expMask),
                           builder.getInt64(0)),
      builder.CreateICmpNE(
          builder.CreateAnd(bits,
                            builder.getInt64(0x000FFFFFFFFFFFFFull)),
          builder.getInt64(0)));
}

Value *nonFinite(IRBuilder<> &builder, Value *bits) {
  const auto expMask = builder.getInt64(0x7FF0000000000000ull);
  return builder.CreateICmpEQ(builder.CreateAnd(bits, expMask), expMask);
}

Value *pairedSingle(IRBuilder<> &builder, Value *ni, Value *result,
                    Value *resultBits) {
  Type *f32 = Type::getFloatTy(builder.getContext());
  Value *rounded = builder.CreateFPTrunc(result, f32);
  Value *magnitude = builder.CreateAnd(
      resultBits, builder.getInt64(0x7FFFFFFFFFFFFFFFull));
  Value *subnormal = builder.CreateICmpULT(
      magnitude, builder.getInt64(0x3810000000000000ull));
  Value *flush = builder.CreateAnd(ni, subnormal);
  Value *sign = builder.CreateTrunc(
      builder.CreateLShr(
          builder.CreateAnd(resultBits,
                            builder.getInt64(0x8000000000000000ull)),
          builder.getInt64(32)),
      Type::getInt32Ty(builder.getContext()));
  Value *signedZero = builder.CreateBitCast(sign, f32);
  return builder.CreateSelect(flush, signedZero, rounded);
}

Value *classifyPairedSingle(IRBuilder<> &builder, Value *value) {
  Value *bits =
      builder.CreateBitCast(value, Type::getInt32Ty(builder.getContext()));
  Value *sign = builder.CreateICmpNE(
      builder.CreateAnd(bits, builder.getInt32(0x80000000u)),
      builder.getInt32(0));
  Value *exponent = builder.CreateAnd(bits, builder.getInt32(0x7F800000u));
  Value *fraction = builder.CreateAnd(bits, builder.getInt32(0x007FFFFFu));
  Value *hasFraction =
      builder.CreateICmpNE(fraction, builder.getInt32(0));
  Value *isInfiniteOrNaN =
      builder.CreateICmpEQ(exponent, builder.getInt32(0x7F800000u));
  Value *isZeroOrDenormal =
      builder.CreateICmpEQ(exponent, builder.getInt32(0));

  Value *infinite = builder.CreateSelect(sign, builder.getInt32(0x09u),
                                         builder.getInt32(0x05u));
  Value *infiniteOrNaN =
      builder.CreateSelect(hasFraction, builder.getInt32(0x11u), infinite);
  Value *zero = builder.CreateSelect(sign, builder.getInt32(0x12u),
                                     builder.getInt32(0x02u));
  Value *denormal = builder.CreateSelect(sign, builder.getInt32(0x18u),
                                         builder.getInt32(0x14u));
  Value *zeroOrDenormal = builder.CreateSelect(hasFraction, denormal, zero);
  Value *normal = builder.CreateSelect(sign, builder.getInt32(0x08u),
                                       builder.getInt32(0x04u));
  return builder.CreateSelect(
      isInfiniteOrNaN, infiniteOrNaN,
      builder.CreateSelect(isZeroOrDenormal, zeroOrDenormal, normal));
}

Value *pairedMaddUnusual(IRBuilder<> &builder, Value *cBits,
                         Value *result0Bits, Value *result1Bits) {
  auto resultUnusual = [&builder](Value *bits) {
    Value *tie = builder.CreateICmpEQ(
        builder.CreateAnd(bits, builder.getInt64(0x1FFFFFFFull)),
        builder.getInt64(0x10000000ull));
    return builder.CreateOr(tie, nonFinite(builder, bits));
  };
  return builder.CreateOr(
      pairedCUnusual(builder, cBits),
      builder.CreateOr(resultUnusual(result0Bits),
                       resultUnusual(result1Bits)));
}

} // namespace

AllocaInst *FunctionEmitter::temporary(Type *valueType, StringRef name) {
  IRBuilder<> allocations(entry_->getTerminator());
  return allocations.CreateAlloca(valueType, nullptr, name);
}

Value *FunctionEmitter::stateValue(DolIRStateSlot slot) {
  return builder_.CreateLoad(type(dolir_state_type(slot)), state_[slot]);
}

void FunctionEmitter::syncState(DolIRStateSlot slot) {
  storeContext(slot, stateValue(slot));
}

void FunctionEmitter::reloadState(DolIRStateSlot slot) {
  builder_.CreateStore(loadContext(slot), state_[slot]);
}

void FunctionEmitter::reloadUsedState() {
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (used_[slot])
      reloadState(static_cast<DolIRStateSlot>(slot));
  }
  builder_.CreateStore(builder_.getInt64(0), cycles_);
}

void FunctionEmitter::continueAfterRuntimeBoundary(StringRef prefix) {
  Value *exception =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, exception));
  BasicBlock *resume =
      BasicBlock::Create(context_, prefix + "_resume", function_);
  BasicBlock *failed =
      BasicBlock::Create(context_, prefix + "_exit", function_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(exception, builder_.getInt32(0)),
                        resume, failed);
  builder_.SetInsertPoint(failed);
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(resume);
  reloadUsedState();
}

void FunctionEmitter::emitFPSCRUpdated() {
  syncState(DOLIR_STATE_FPSCR);
  auto callee = module_.getOrInsertFunction(
      "ppc_fpscr_control_updated",
      FunctionType::get(Type::getVoidTy(context_),
                        {PointerType::getUnqual(context_)}, false));
  builder_.CreateCall(callee, {ctx_});
  reloadState(DOLIR_STATE_FPSCR);
}

void FunctionEmitter::emitFPSCRBit(u64 descriptor) {
  syncState(DOLIR_STATE_FPSCR);
  const char *name =
      ((descriptor >> 8) & 1u) ? "ppc_mtfsb1_op" : "ppc_mtfsb0_op";
  auto callee = module_.getOrInsertFunction(
      name, FunctionType::get(
                Type::getVoidTy(context_),
                {PointerType::getUnqual(context_), Type::getInt8Ty(context_)},
                false));
  builder_.CreateCall(callee, {ctx_, builder_.getInt8(descriptor & 0xFFu)});
  reloadState(DOLIR_STATE_FPSCR);
}

void FunctionEmitter::emitProgramException(const DolIRInstruction &inst) {
  BasicBlock *taken = BasicBlock::Create(context_, "trap_taken", function_);
  BasicBlock *resume = BasicBlock::Create(context_, "trap_resume", function_);
  builder_.CreateCondBr(operand(inst, 0), taken, resume);
  builder_.SetInsertPoint(taken);
  materialize(inst.guest_pc);
  auto callee = module_.getOrInsertFunction(
      "ppc_program_exception",
      FunctionType::get(Type::getVoidTy(context_),
                        {PointerType::getUnqual(context_),
                         Type::getInt32Ty(context_),
                         Type::getInt32Ty(context_)},
                        false));
  builder_.CreateCall(callee, {ctx_, builder_.getInt32(inst.immediate),
                               builder_.getInt32(inst.guest_pc)});
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(resume);
}

Value *FunctionEmitter::emitSPRRead(const DolIRInstruction &inst) {
  materialize(inst.guest_pc);
  auto callee = module_.getOrInsertFunction(
      "ppc_mfspr", FunctionType::get(Type::getInt32Ty(context_),
                                     {PointerType::getUnqual(context_),
                                      Type::getInt16Ty(context_),
                                      Type::getInt32Ty(context_)},
                                     false));
  Value *result =
      builder_.CreateCall(callee, {ctx_, builder_.getInt16(inst.immediate),
                                   builder_.getInt32(inst.guest_pc)});
  continueAfterRuntimeBoundary("mfspr");
  return result;
}

void FunctionEmitter::emitSPRWrite(const DolIRInstruction &inst) {
  materialize(inst.guest_pc);
  auto callee = module_.getOrInsertFunction(
      "ppc_mtspr",
      FunctionType::get(Type::getVoidTy(context_),
                        {PointerType::getUnqual(context_),
                         Type::getInt16Ty(context_), Type::getInt32Ty(context_),
                         Type::getInt32Ty(context_)},
                        false));
  builder_.CreateCall(callee,
                      {ctx_, builder_.getInt16(inst.immediate),
                       operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  continueAfterRuntimeBoundary("mtspr");
}

void FunctionEmitter::emitLSWX(const DolIRInstruction &inst) {
  materialize(inst.guest_pc);
  auto callee = module_.getOrInsertFunction(
      "ppc_lswx",
      FunctionType::get(Type::getVoidTy(context_),
                        {PointerType::getUnqual(context_),
                         Type::getInt8Ty(context_), Type::getInt8Ty(context_),
                         Type::getInt8Ty(context_), Type::getInt32Ty(context_)},
                        false));
  builder_.CreateCall(callee, {ctx_, builder_.getInt8(inst.immediate & 0xFFu),
                               builder_.getInt8((inst.immediate >> 8) & 0xFFu),
                               builder_.getInt8((inst.immediate >> 16) & 0xFFu),
                               builder_.getInt32(inst.guest_pc)});
  continueAfterRuntimeBoundary("lswx");
}

Value *FunctionEmitter::emitRuntimeBoundary(const DolIRInstruction &inst) {
  materialize(inst.guest_pc);
  Type *ptr = PointerType::getUnqual(context_);
  Value *result = nullptr;
  StringRef prefix;
  if (inst.aux == DOLIR_HELPER_DCBZ_L) {
    prefix = "dcbz_l";
    auto callee = module_.getOrInsertFunction(
        "ppc_dcbz_l", FunctionType::get(Type::getVoidTy(context_),
                                        {ptr, Type::getInt32Ty(context_),
                                         Type::getInt32Ty(context_)},
                                        false));
    builder_.CreateCall(
        callee, {ctx_, operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  } else if (inst.aux == DOLIR_HELPER_ECIWX) {
    prefix = "eciwx";
    auto callee = module_.getOrInsertFunction(
        "ppc_eciwx", FunctionType::get(Type::getInt32Ty(context_),
                                       {ptr, Type::getInt32Ty(context_),
                                        Type::getInt32Ty(context_)},
                                       false));
    result = builder_.CreateCall(
        callee, {ctx_, operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  } else if (inst.aux == DOLIR_HELPER_ECOWX) {
    prefix = "ecowx";
    auto callee = module_.getOrInsertFunction(
        "ppc_ecowx", FunctionType::get(Type::getVoidTy(context_),
                                       {ptr, Type::getInt32Ty(context_),
                                        Type::getInt32Ty(context_),
                                        Type::getInt32Ty(context_)},
                                       false));
    builder_.CreateCall(callee, {ctx_, operand(inst, 0), operand(inst, 1),
                                 builder_.getInt32(inst.guest_pc)});
  } else if (inst.aux == DOLIR_HELPER_TLBIE) {
    prefix = "tlbie";
    auto callee = module_.getOrInsertFunction(
        "ppc_tlbie", FunctionType::get(Type::getVoidTy(context_),
                                       {ptr, Type::getInt32Ty(context_),
                                        Type::getInt32Ty(context_)},
                                       false));
    builder_.CreateCall(
        callee, {ctx_, operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  } else {
    prefix = "cache";
    auto callee = module_.getOrInsertFunction(
        "ppc_cache_control", FunctionType::get(Type::getVoidTy(context_),
                                               {ptr, Type::getInt8Ty(context_),
                                                Type::getInt32Ty(context_),
                                                Type::getInt32Ty(context_)},
                                               false));
    builder_.CreateCall(callee,
                        {ctx_, builder_.getInt8(inst.immediate),
                         operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  }
  continueAfterRuntimeBoundary(prefix);
  return result;
}

void FunctionEmitter::emitExactFloat(u64 descriptor) {
  auto op = static_cast<DolIRExactFloat>(descriptor & 0xFFu);
  u32 d = (descriptor >> 8) & 0xFFu;
  u32 a = (descriptor >> 16) & 0xFFu;
  u32 b = (descriptor >> 24) & 0xFFu;
  u32 c = (descriptor >> 32) & 0xFFu;
  u32 crfd = (descriptor >> 40) & 0xFFu;
  auto fprSlot = [](u32 reg) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_FPR0 + reg);
  };
  auto ps1Slot = [](u32 reg) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_PS1_0 + reg);
  };
  Type *ptr = PointerType::getUnqual(context_);
  Type *f64 = Type::getDoubleTy(context_);

  if (op == DOLIR_EXACT_FCMPU || op == DOLIR_EXACT_FCMPO) {
    syncState(DOLIR_STATE_FPSCR);
    syncState(DOLIR_STATE_CR);
    auto callee = module_.getOrInsertFunction(
        "ppc_fcmp", FunctionType::get(Type::getVoidTy(context_),
                                      {ptr, Type::getInt8Ty(context_), f64, f64,
                                       Type::getInt1Ty(context_)},
                                      false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(crfd),
                                 stateValue(fprSlot(a)), stateValue(fprSlot(b)),
                                 builder_.getInt1(op == DOLIR_EXACT_FCMPO)});
    reloadState(DOLIR_STATE_CR);
    reloadState(DOLIR_STATE_FPSCR);
    return;
  }

  DolIRStateSlot destination = fprSlot(d);
  Value *old = stateValue(destination);

  // The ordinary finite cases can operate directly on the generated SSA
  // register state. Besides eliminating the call itself, this avoids spilling
  // every operand to CPUState merely so a helper can select constant indices.
  if (op == DOLIR_EXACT_FADDS || op == DOLIR_EXACT_FSUBS ||
      op == DOLIR_EXACT_FMULS) {
    const bool multiply = op == DOLIR_EXACT_FMULS;
    Value *lhs = stateValue(fprSlot(a));
    Value *rhs = stateValue(fprSlot(multiply ? c : b));
    Value *rhsBits =
        builder_.CreateBitCast(rhs, Type::getInt64Ty(context_));
    if (multiply)
      rhs = builder_.CreateBitCast(roundPairedC(builder_, rhsBits), f64);
    Value *result = op == DOLIR_EXACT_FADDS
                        ? builder_.CreateFAdd(lhs, rhs)
                    : op == DOLIR_EXACT_FSUBS
                        ? builder_.CreateFSub(lhs, rhs)
                        : builder_.CreateFMul(lhs, rhs);
    Value *resultBits =
        builder_.CreateBitCast(result, Type::getInt64Ty(context_));
    Value *unusual = nonFinite(builder_, resultBits);
    if (multiply)
      unusual = builder_.CreateOr(unusual,
                                  pairedCUnusual(builder_, rhsBits));

    BasicBlock *fast =
        BasicBlock::Create(context_, "scalar_single_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "scalar_single_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "scalar_single_done", function_);
    builder_.CreateCondBr(unusual, slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single = pairedSingle(builder_, ni, result, resultBits);
    Value *extended = builder_.CreateFPExt(single, f64);
    builder_.CreateStore(extended, state_[destination]);
    builder_.CreateStore(extended, state_[ps1Slot(d)]);
    Value *preserveMask = builder_.getInt32(
        multiply ? ~(0x00060000u | (0x1Fu << 12)) : ~(0x1Fu << 12));
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, preserveMask),
        builder_.CreateShl(classifyPairedSingle(builder_, single),
                           builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncState(destination);
    syncState(ps1Slot(d));
    syncState(fprSlot(a));
    syncState(fprSlot(multiply ? c : b));
    const char *name = op == DOLIR_EXACT_FADDS   ? "ppc_fadds"
                       : op == DOLIR_EXACT_FSUBS ? "ppc_fsubs"
                                                 : "ppc_fmuls";
    auto callee = module_.getOrInsertFunction(
        name, FunctionType::get(Type::getVoidTy(context_),
                                {ptr, Type::getInt8Ty(context_),
                                 Type::getInt8Ty(context_),
                                 Type::getInt8Ty(context_)},
                                false));
    builder_.CreateCall(callee,
                        {ctx_, builder_.getInt8(d), builder_.getInt8(a),
                         builder_.getInt8(multiply ? c : b)});
    reloadState(destination);
    reloadState(ps1Slot(d));
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  if (op >= DOLIR_EXACT_FMADDS && op <= DOLIR_EXACT_FNMSUBS) {
    const bool subtract =
        op == DOLIR_EXACT_FMSUBS || op == DOLIR_EXACT_FNMSUBS;
    const bool negative =
        op == DOLIR_EXACT_FNMADDS || op == DOLIR_EXACT_FNMSUBS;
    Value *lhs = stateValue(fprSlot(a));
    Value *cValue = stateValue(fprSlot(c));
    Value *addend = stateValue(fprSlot(b));
    Value *cBits =
        builder_.CreateBitCast(cValue, Type::getInt64Ty(context_));
    Value *roundedC =
        builder_.CreateBitCast(roundPairedC(builder_, cBits), f64);
    if (subtract)
      addend = builder_.CreateFNeg(addend);
    Value *result = emitPairedFMA(builder_, module_, lhs, roundedC, addend);
    if (negative)
      result = builder_.CreateFNeg(result);
    Value *resultBits =
        builder_.CreateBitCast(result, Type::getInt64Ty(context_));
    Value *unusual = pairedMaddUnusual(builder_, cBits, resultBits,
                                       resultBits);

    BasicBlock *fast =
        BasicBlock::Create(context_, "scalar_madd_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "scalar_madd_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "scalar_madd_done", function_);
    builder_.CreateCondBr(unusual, slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single = pairedSingle(builder_, ni, result, resultBits);
    Value *extended = builder_.CreateFPExt(single, f64);
    builder_.CreateStore(extended, state_[destination]);
    builder_.CreateStore(extended, state_[ps1Slot(d)]);
    Value *preserveMask = builder_.getInt32(
        subtract || negative ? ~(0x1Fu << 12)
                             : ~(0x00060000u | (0x1Fu << 12)));
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, preserveMask),
        builder_.CreateShl(classifyPairedSingle(builder_, single),
                           builder_.getInt32(12)));
    if (!subtract && !negative) {
      Value *inexact = builder_.CreateFCmpUNE(result, extended);
      updatedFPSCR = builder_.CreateOr(
          updatedFPSCR,
          builder_.CreateSelect(inexact, builder_.getInt32(0x00020000u),
                                builder_.getInt32(0)));
    }
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncState(destination);
    syncState(ps1Slot(d));
    syncState(fprSlot(a));
    syncState(fprSlot(b));
    syncState(fprSlot(c));
    AllocaInst *output = temporary(f64, "fma.result");
    builder_.CreateStore(old, output);
    auto callee = module_.getOrInsertFunction(
        "ppc_fma",
        FunctionType::get(Type::getInt1Ty(context_),
                          {ptr, f64, f64, f64, Type::getInt1Ty(context_),
                           Type::getInt1Ty(context_), Type::getInt1Ty(context_),
                           ptr},
                          false));
    Value *success = builder_.CreateCall(
        callee,
        {ctx_, stateValue(fprSlot(a)), stateValue(fprSlot(c)),
         stateValue(fprSlot(b)), builder_.getInt1(true),
         builder_.getInt1(subtract), builder_.getInt1(negative), output});
    Value *fused = builder_.CreateLoad(f64, output);
    builder_.CreateStore(builder_.CreateSelect(success, fused, old),
                         state_[destination]);
    Value *oldPs1 = stateValue(ps1Slot(d));
    builder_.CreateStore(builder_.CreateSelect(success, fused, oldPs1),
                         state_[ps1Slot(d)]);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  if (op >= DOLIR_EXACT_FADDS && op <= DOLIR_EXACT_FRSP) {
    syncState(DOLIR_STATE_FPSCR);
    const char *name = nullptr;
    switch (op) {
    case DOLIR_EXACT_FADDS:
      name = "ppc_fadds";
      break;
    case DOLIR_EXACT_FSUBS:
      name = "ppc_fsubs";
      break;
    case DOLIR_EXACT_FMULS:
      name = "ppc_fmuls";
      break;
    case DOLIR_EXACT_FDIVS:
      name = "ppc_fdivs";
      break;
    case DOLIR_EXACT_FADD:
      name = "ppc_fadd";
      break;
    case DOLIR_EXACT_FSUB:
      name = "ppc_fsub";
      break;
    case DOLIR_EXACT_FMUL:
      name = "ppc_fmul";
      break;
    case DOLIR_EXACT_FDIV:
      name = "ppc_fdiv";
      break;
    case DOLIR_EXACT_FRSP:
      name = "ppc_frsp";
      break;
    default:
      break;
    }
    syncState(destination);
    bool single = op <= DOLIR_EXACT_FDIVS || op == DOLIR_EXACT_FRSP;
    if (single)
      syncState(ps1Slot(d));
    syncState(fprSlot(op == DOLIR_EXACT_FRSP ? b : a));
    if (op != DOLIR_EXACT_FRSP)
      syncState(
          fprSlot(op == DOLIR_EXACT_FMULS || op == DOLIR_EXACT_FMUL ? c : b));
    if (op == DOLIR_EXACT_FRSP) {
      auto callee = module_.getOrInsertFunction(
          name, FunctionType::get(
                    Type::getVoidTy(context_),
                    {ptr, Type::getInt8Ty(context_), Type::getInt8Ty(context_)},
                    false));
      builder_.CreateCall(callee,
                          {ctx_, builder_.getInt8(d), builder_.getInt8(b)});
    } else {
      auto callee = module_.getOrInsertFunction(
          name, FunctionType::get(Type::getVoidTy(context_),
                                  {ptr, Type::getInt8Ty(context_),
                                   Type::getInt8Ty(context_),
                                   Type::getInt8Ty(context_)},
                                  false));
      builder_.CreateCall(
          callee,
          {ctx_, builder_.getInt8(d), builder_.getInt8(a),
           builder_.getInt8(
               op == DOLIR_EXACT_FMULS || op == DOLIR_EXACT_FMUL ? c : b)});
    }
    reloadState(destination);
    if (single)
      reloadState(ps1Slot(d));
  } else if (op == DOLIR_EXACT_FCTIW || op == DOLIR_EXACT_FCTIWZ) {
    syncState(DOLIR_STATE_FPSCR);
    AllocaInst *output = temporary(Type::getInt64Ty(context_), "fctiw.result");
    builder_.CreateStore(
        builder_.CreateBitCast(old, Type::getInt64Ty(context_)), output);
    auto callee = module_.getOrInsertFunction(
        "ppc_fctiw",
        FunctionType::get(Type::getInt1Ty(context_),
                          {ptr, f64, Type::getInt1Ty(context_), ptr}, false));
    Value *success = builder_.CreateCall(
        callee, {ctx_, stateValue(fprSlot(b)),
                 builder_.getInt1(op == DOLIR_EXACT_FCTIWZ), output});
    Value *converted = builder_.CreateBitCast(
        builder_.CreateLoad(Type::getInt64Ty(context_), output), f64);
    builder_.CreateStore(builder_.CreateSelect(success, converted, old),
                         state_[destination]);
  } else if (op == DOLIR_EXACT_FRES || op == DOLIR_EXACT_FRSQRTE) {
    syncState(DOLIR_STATE_FPSCR);
    AllocaInst *output = temporary(f64, "estimate.result");
    builder_.CreateStore(old, output);
    const char *name = op == DOLIR_EXACT_FRES ? "ppc_fres" : "ppc_frsqrte";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getInt1Ty(context_), {ptr, f64, ptr}, false));
    Value *success =
        builder_.CreateCall(callee, {ctx_, stateValue(fprSlot(b)), output});
    Value *estimate = builder_.CreateLoad(f64, output);
    builder_.CreateStore(builder_.CreateSelect(success, estimate, old),
                         state_[destination]);
    if (op == DOLIR_EXACT_FRES) {
      DolIRStateSlot ps1 = ps1Slot(d);
      Value *oldPs1 = stateValue(ps1);
      builder_.CreateStore(builder_.CreateSelect(success, estimate, oldPs1),
                           state_[ps1]);
    }
  } else {
    syncState(DOLIR_STATE_FPSCR);
    bool single = op >= DOLIR_EXACT_FMADDS && op <= DOLIR_EXACT_FNMSUBS;
    bool subtract = op == DOLIR_EXACT_FMSUB || op == DOLIR_EXACT_FNMSUB ||
                    op == DOLIR_EXACT_FMSUBS || op == DOLIR_EXACT_FNMSUBS;
    bool negative = op == DOLIR_EXACT_FNMADD || op == DOLIR_EXACT_FNMSUB ||
                    op == DOLIR_EXACT_FNMADDS || op == DOLIR_EXACT_FNMSUBS;
    AllocaInst *output = temporary(f64, "fma.result");
    builder_.CreateStore(old, output);
    auto callee = module_.getOrInsertFunction(
        "ppc_fma",
        FunctionType::get(Type::getInt1Ty(context_),
                          {ptr, f64, f64, f64, Type::getInt1Ty(context_),
                           Type::getInt1Ty(context_), Type::getInt1Ty(context_),
                           ptr},
                          false));
    Value *success = builder_.CreateCall(
        callee,
        {ctx_, stateValue(fprSlot(a)), stateValue(fprSlot(c)),
         stateValue(fprSlot(b)), builder_.getInt1(single),
         builder_.getInt1(subtract), builder_.getInt1(negative), output});
    Value *fused = builder_.CreateLoad(f64, output);
    builder_.CreateStore(builder_.CreateSelect(success, fused, old),
                         state_[destination]);
    if (single) {
      DolIRStateSlot ps1 = ps1Slot(d);
      Value *oldPs1 = stateValue(ps1);
      builder_.CreateStore(builder_.CreateSelect(success, fused, oldPs1),
                           state_[ps1]);
    }
  }
  reloadState(DOLIR_STATE_FPSCR);
}

void FunctionEmitter::emitExactPaired(u64 descriptor) {
  auto op = static_cast<DolIRExactPaired>(descriptor & 0xFFu);
  u32 d = (descriptor >> 8) & 0xFFu;
  u32 a = (descriptor >> 16) & 0xFFu;
  u32 b = (descriptor >> 24) & 0xFFu;
  u32 c = (descriptor >> 32) & 0xFFu;
  u32 crfd = (descriptor >> 40) & 0xFFu;
  auto fprSlot = [](u32 reg) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_FPR0 + reg);
  };
  auto ps1Slot = [](u32 reg) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_PS1_0 + reg);
  };
  auto syncPair = [this, &fprSlot, &ps1Slot](u32 reg) {
    syncState(fprSlot(reg));
    syncState(ps1Slot(reg));
  };
  auto reloadPair = [this, &fprSlot, &ps1Slot](u32 reg) {
    reloadState(fprSlot(reg));
    reloadState(ps1Slot(reg));
  };
  Type *ptr = PointerType::getUnqual(context_);
  Type *i8 = Type::getInt8Ty(context_);
  Type *f64 = Type::getDoubleTy(context_);

  if (op >= DOLIR_EXACT_PS_CMPU0) {
    syncState(DOLIR_STATE_FPSCR);
    bool lane1 = op == DOLIR_EXACT_PS_CMPU1 || op == DOLIR_EXACT_PS_CMPO1;
    bool ordered = op == DOLIR_EXACT_PS_CMPO0 || op == DOLIR_EXACT_PS_CMPO1;
    syncState(DOLIR_STATE_CR);
    syncPair(a);
    syncPair(b);
    auto callee = module_.getOrInsertFunction(
        "ppc_fcmp", FunctionType::get(
                        Type::getVoidTy(context_),
                        {ptr, i8, f64, f64, Type::getInt1Ty(context_)}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(crfd),
                                 stateValue(lane1 ? ps1Slot(a) : fprSlot(a)),
                                 stateValue(lane1 ? ps1Slot(b) : fprSlot(b)),
                                 builder_.getInt1(ordered)});
    reloadState(DOLIR_STATE_CR);
    reloadState(DOLIR_STATE_FPSCR);
    return;
  }

  if (op == DOLIR_EXACT_PS_MADD || op == DOLIR_EXACT_PS_MSUB ||
      op == DOLIR_EXACT_PS_NMADD || op == DOLIR_EXACT_PS_NMSUB) {
    const bool subtract =
        op == DOLIR_EXACT_PS_MSUB || op == DOLIR_EXACT_PS_NMSUB;
    const bool negative =
        op == DOLIR_EXACT_PS_NMADD || op == DOLIR_EXACT_PS_NMSUB;
    Value *a0 = stateValue(fprSlot(a));
    Value *a1 = stateValue(ps1Slot(a));
    Value *c0 = stateValue(fprSlot(c));
    Value *c1 = stateValue(ps1Slot(c));
    Value *b0 = stateValue(fprSlot(b));
    Value *b1 = stateValue(ps1Slot(b));
    Value *c0Bits = builder_.CreateBitCast(c0, Type::getInt64Ty(context_));
    Value *c1Bits = builder_.CreateBitCast(c1, Type::getInt64Ty(context_));
    Value *roundedC0 =
        builder_.CreateBitCast(roundPairedC(builder_, c0Bits), f64);
    Value *roundedC1 =
        builder_.CreateBitCast(roundPairedC(builder_, c1Bits), f64);
    if (subtract) {
      b0 = builder_.CreateFNeg(b0);
      b1 = builder_.CreateFNeg(b1);
    }
    Value *result0 = emitPairedFMA(builder_, module_, a0, roundedC0, b0);
    Value *result1 = emitPairedFMA(builder_, module_, a1, roundedC1, b1);
    if (negative) {
      result0 = builder_.CreateFNeg(result0);
      result1 = builder_.CreateFNeg(result1);
    }
    Value *result0Bits =
        builder_.CreateBitCast(result0, Type::getInt64Ty(context_));
    Value *result1Bits =
        builder_.CreateBitCast(result1, Type::getInt64Ty(context_));
    Value *unusual = builder_.CreateOr(
        pairedMaddUnusual(builder_, c0Bits, result0Bits, result1Bits),
        pairedCUnusual(builder_, c1Bits));

    BasicBlock *fast =
        BasicBlock::Create(context_, "ps_madd_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "ps_madd_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "ps_madd_done", function_);
    builder_.CreateCondBr(unusual, slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single0 = pairedSingle(builder_, ni, result0, result0Bits);
    Value *single1 = pairedSingle(builder_, ni, result1, result1Bits);
    builder_.CreateStore(builder_.CreateFPExt(single0, f64),
                         state_[fprSlot(d)]);
    builder_.CreateStore(builder_.CreateFPExt(single1, f64),
                         state_[ps1Slot(d)]);
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, builder_.getInt32(~(0x1Fu << 12))),
        builder_.CreateShl(classifyPairedSingle(builder_, single0),
                           builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncPair(d);
    syncPair(a);
    syncPair(b);
    syncPair(c);
    auto callee = module_.getOrInsertFunction(
        "ppc_ps_madd_op",
        FunctionType::get(Type::getVoidTy(context_),
                          {ptr, i8, i8, i8, i8, Type::getInt1Ty(context_),
                           Type::getInt1Ty(context_)},
                          false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d),
                                 builder_.getInt8(a), builder_.getInt8(c),
                                 builder_.getInt8(b),
                                 builder_.getInt1(subtract),
                                 builder_.getInt1(negative)});
    reloadPair(d);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  // These two scalar-C forms dominate paired-single work in real games. Keep
  // their ordinary finite path in SSA so constants register-select the source
  // slots and the result never makes a round trip through CPUState. The rare
  // cases which need exception bookkeeping or a fused tie correction retain
  // the existing exact helper path.
  if (op == DOLIR_EXACT_PS_MADDS0 || op == DOLIR_EXACT_PS_MADDS1) {
    Value *a0 = stateValue(fprSlot(a));
    Value *a1 = stateValue(ps1Slot(a));
    Value *cValue =
        stateValue(op == DOLIR_EXACT_PS_MADDS0 ? fprSlot(c) : ps1Slot(c));
    Value *b0 = stateValue(fprSlot(b));
    Value *b1 = stateValue(ps1Slot(b));
    Value *cBits =
        builder_.CreateBitCast(cValue, Type::getInt64Ty(context_));
    Value *roundedCBits = roundPairedC(builder_, cBits);
    Value *roundedC = builder_.CreateBitCast(roundedCBits, f64);
    Value *result0 = emitPairedFMA(builder_, module_, a0, roundedC, b0);
    Value *result1 = emitPairedFMA(builder_, module_, a1, roundedC, b1);
    Value *result0Bits =
        builder_.CreateBitCast(result0, Type::getInt64Ty(context_));
    Value *result1Bits =
        builder_.CreateBitCast(result1, Type::getInt64Ty(context_));

    BasicBlock *fast =
        BasicBlock::Create(context_, "ps_madds_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "ps_madds_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "ps_madds_done", function_);
    builder_.CreateCondBr(pairedMaddUnusual(builder_, cBits, result0Bits,
                                            result1Bits),
                          slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single0 = pairedSingle(builder_, ni, result0, result0Bits);
    Value *single1 = pairedSingle(builder_, ni, result1, result1Bits);
    builder_.CreateStore(builder_.CreateFPExt(single0, f64),
                         state_[fprSlot(d)]);
    builder_.CreateStore(builder_.CreateFPExt(single1, f64),
                         state_[ps1Slot(d)]);
    Value *classification = classifyPairedSingle(builder_, single0);
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, builder_.getInt32(~(0x1Fu << 12))),
        builder_.CreateShl(classification, builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncPair(d);
    syncPair(a);
    syncPair(b);
    syncPair(c);
    const char *name = op == DOLIR_EXACT_PS_MADDS0 ? "ppc_ps_madds0"
                                                    : "ppc_ps_madds1";
    auto callee = module_.getOrInsertFunction(
        name, FunctionType::get(Type::getVoidTy(context_),
                                {ptr, i8, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d),
                                 builder_.getInt8(a), builder_.getInt8(c),
                                 builder_.getInt8(b)});
    reloadPair(d);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  if (op == DOLIR_EXACT_PS_MULS0 || op == DOLIR_EXACT_PS_MULS1) {
    Value *a0 = stateValue(fprSlot(a));
    Value *a1 = stateValue(ps1Slot(a));
    Value *cValue =
        stateValue(op == DOLIR_EXACT_PS_MULS0 ? fprSlot(c) : ps1Slot(c));
    Value *cBits =
        builder_.CreateBitCast(cValue, Type::getInt64Ty(context_));
    Value *roundedC =
        builder_.CreateBitCast(roundPairedC(builder_, cBits), f64);
    Value *result0 = builder_.CreateFMul(a0, roundedC);
    Value *result1 = builder_.CreateFMul(a1, roundedC);
    Value *result0Bits =
        builder_.CreateBitCast(result0, Type::getInt64Ty(context_));
    Value *result1Bits =
        builder_.CreateBitCast(result1, Type::getInt64Ty(context_));
    Value *unusual = builder_.CreateOr(
        pairedCUnusual(builder_, cBits),
        builder_.CreateOr(nonFinite(builder_, result0Bits),
                          nonFinite(builder_, result1Bits)));

    BasicBlock *fast =
        BasicBlock::Create(context_, "ps_muls_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "ps_muls_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "ps_muls_done", function_);
    builder_.CreateCondBr(unusual, slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single0 = pairedSingle(builder_, ni, result0, result0Bits);
    Value *single1 = pairedSingle(builder_, ni, result1, result1Bits);
    builder_.CreateStore(builder_.CreateFPExt(single0, f64),
                         state_[fprSlot(d)]);
    builder_.CreateStore(builder_.CreateFPExt(single1, f64),
                         state_[ps1Slot(d)]);
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, builder_.getInt32(~(0x1Fu << 12))),
        builder_.CreateShl(classifyPairedSingle(builder_, single0),
                           builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncPair(d);
    syncPair(a);
    syncPair(c);
    const char *name =
        op == DOLIR_EXACT_PS_MULS0 ? "ppc_ps_muls0" : "ppc_ps_muls1";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getVoidTy(context_), {ptr, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d),
                                 builder_.getInt8(a), builder_.getInt8(c)});
    reloadPair(d);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  // ps_sum only computes one lane; the other is a rounded copy from C. Keep
  // the ordinary finite sum in SSA and preserve the exact helper for the rare
  // non-finite case that needs PowerPC exception behavior.
  if (op == DOLIR_EXACT_PS_SUM0 || op == DOLIR_EXACT_PS_SUM1) {
    const bool sum1 = op == DOLIR_EXACT_PS_SUM1;
    Value *sum = builder_.CreateFAdd(stateValue(fprSlot(a)),
                                     stateValue(ps1Slot(b)));
    Value *sumBits =
        builder_.CreateBitCast(sum, Type::getInt64Ty(context_));

    BasicBlock *fast =
        BasicBlock::Create(context_, "ps_sum_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "ps_sum_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "ps_sum_done", function_);
    builder_.CreateCondBr(nonFinite(builder_, sumBits), slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *copy = stateValue(sum1 ? fprSlot(c) : ps1Slot(c));
    Value *copyBits =
        builder_.CreateBitCast(copy, Type::getInt64Ty(context_));
    Value *singleSum = pairedSingle(builder_, ni, sum, sumBits);
    Value *singleCopy = pairedSingle(builder_, ni, copy, copyBits);
    builder_.CreateStore(builder_.CreateFPExt(sum1 ? singleCopy : singleSum,
                                              f64),
                         state_[fprSlot(d)]);
    builder_.CreateStore(builder_.CreateFPExt(sum1 ? singleSum : singleCopy,
                                              f64),
                         state_[ps1Slot(d)]);
    // ps_sum1 reports the lane it computes, rather than destination lane 0.
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, builder_.getInt32(~(0x1Fu << 12))),
        builder_.CreateShl(classifyPairedSingle(builder_, singleSum),
                           builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncPair(d);
    syncPair(a);
    syncPair(b);
    syncPair(c);
    const char *name = sum1 ? "ppc_ps_sum1" : "ppc_ps_sum0";
    auto callee = module_.getOrInsertFunction(
        name, FunctionType::get(Type::getVoidTy(context_),
                                {ptr, i8, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d),
                                 builder_.getInt8(a), builder_.getInt8(c),
                                 builder_.getInt8(b)});
    reloadPair(d);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  if (op == DOLIR_EXACT_PS_ADD || op == DOLIR_EXACT_PS_SUB ||
      op == DOLIR_EXACT_PS_MUL) {
    const bool multiply = op == DOLIR_EXACT_PS_MUL;
    Value *lhs0 = stateValue(fprSlot(a));
    Value *lhs1 = stateValue(ps1Slot(a));
    const u32 rhsRegister = multiply ? c : b;
    Value *rhs0 = stateValue(fprSlot(rhsRegister));
    Value *rhs1 = stateValue(ps1Slot(rhsRegister));
    Value *rhs0Bits =
        builder_.CreateBitCast(rhs0, Type::getInt64Ty(context_));
    Value *rhs1Bits =
        builder_.CreateBitCast(rhs1, Type::getInt64Ty(context_));
    if (multiply) {
      rhs0 = builder_.CreateBitCast(roundPairedC(builder_, rhs0Bits), f64);
      rhs1 = builder_.CreateBitCast(roundPairedC(builder_, rhs1Bits), f64);
    }
    Value *result0 = op == DOLIR_EXACT_PS_ADD
                         ? builder_.CreateFAdd(lhs0, rhs0)
                     : op == DOLIR_EXACT_PS_SUB
                         ? builder_.CreateFSub(lhs0, rhs0)
                         : builder_.CreateFMul(lhs0, rhs0);
    Value *result1 = op == DOLIR_EXACT_PS_ADD
                         ? builder_.CreateFAdd(lhs1, rhs1)
                     : op == DOLIR_EXACT_PS_SUB
                         ? builder_.CreateFSub(lhs1, rhs1)
                         : builder_.CreateFMul(lhs1, rhs1);
    Value *result0Bits =
        builder_.CreateBitCast(result0, Type::getInt64Ty(context_));
    Value *result1Bits =
        builder_.CreateBitCast(result1, Type::getInt64Ty(context_));
    Value *unusual = builder_.CreateOr(nonFinite(builder_, result0Bits),
                                       nonFinite(builder_, result1Bits));
    if (multiply)
      unusual = builder_.CreateOr(
          unusual, builder_.CreateOr(pairedCUnusual(builder_, rhs0Bits),
                                     pairedCUnusual(builder_, rhs1Bits)));

    BasicBlock *fast =
        BasicBlock::Create(context_, "ps_binary_fast", function_);
    BasicBlock *slow =
        BasicBlock::Create(context_, "ps_binary_exact", function_);
    BasicBlock *done =
        BasicBlock::Create(context_, "ps_binary_done", function_);
    builder_.CreateCondBr(unusual, slow, fast);

    builder_.SetInsertPoint(fast);
    Value *fpscr = stateValue(DOLIR_STATE_FPSCR);
    Value *ni = builder_.CreateICmpNE(
        builder_.CreateAnd(fpscr, builder_.getInt32(0x4u)),
        builder_.getInt32(0));
    Value *single0 = pairedSingle(builder_, ni, result0, result0Bits);
    Value *single1 = pairedSingle(builder_, ni, result1, result1Bits);
    builder_.CreateStore(builder_.CreateFPExt(single0, f64),
                         state_[fprSlot(d)]);
    builder_.CreateStore(builder_.CreateFPExt(single1, f64),
                         state_[ps1Slot(d)]);
    Value *updatedFPSCR = builder_.CreateOr(
        builder_.CreateAnd(fpscr, builder_.getInt32(~(0x1Fu << 12))),
        builder_.CreateShl(classifyPairedSingle(builder_, single0),
                           builder_.getInt32(12)));
    builder_.CreateStore(updatedFPSCR, state_[DOLIR_STATE_FPSCR]);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(slow);
    syncState(DOLIR_STATE_FPSCR);
    syncPair(d);
    syncPair(a);
    syncPair(rhsRegister);
    const char *name = op == DOLIR_EXACT_PS_ADD   ? "ppc_ps_add_op"
                       : op == DOLIR_EXACT_PS_SUB ? "ppc_ps_sub_op"
                                                  : "ppc_ps_mul_op";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getVoidTy(context_), {ptr, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d),
                                 builder_.getInt8(a),
                                 builder_.getInt8(rhsRegister)});
    reloadPair(d);
    reloadState(DOLIR_STATE_FPSCR);
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    return;
  }

  syncState(DOLIR_STATE_FPSCR);
  syncPair(d);
  if (op == DOLIR_EXACT_PS_RES || op == DOLIR_EXACT_PS_RSQRTE) {
    syncPair(b);
    const char *name =
        op == DOLIR_EXACT_PS_RES ? "ppc_ps_res_op" : "ppc_ps_rsqrte_op";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getVoidTy(context_), {ptr, i8, i8}, false));
    builder_.CreateCall(callee,
                        {ctx_, builder_.getInt8(d), builder_.getInt8(b)});
  } else if (op == DOLIR_EXACT_PS_MADD || op == DOLIR_EXACT_PS_MSUB ||
             op == DOLIR_EXACT_PS_NMADD || op == DOLIR_EXACT_PS_NMSUB) {
    syncPair(a);
    syncPair(b);
    syncPair(c);
    bool subtract = op == DOLIR_EXACT_PS_MSUB || op == DOLIR_EXACT_PS_NMSUB;
    bool negative = op == DOLIR_EXACT_PS_NMADD || op == DOLIR_EXACT_PS_NMSUB;
    auto callee = module_.getOrInsertFunction(
        "ppc_ps_madd_op",
        FunctionType::get(Type::getVoidTy(context_),
                          {ptr, i8, i8, i8, i8, Type::getInt1Ty(context_),
                           Type::getInt1Ty(context_)},
                          false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d), builder_.getInt8(a),
                                 builder_.getInt8(c), builder_.getInt8(b),
                                 builder_.getInt1(subtract),
                                 builder_.getInt1(negative)});
  } else if (op == DOLIR_EXACT_PS_MADDS0 || op == DOLIR_EXACT_PS_MADDS1 ||
             op == DOLIR_EXACT_PS_SUM0 || op == DOLIR_EXACT_PS_SUM1) {
    syncPair(a);
    syncPair(b);
    syncPair(c);
    const char *name = op == DOLIR_EXACT_PS_MADDS0   ? "ppc_ps_madds0"
                       : op == DOLIR_EXACT_PS_MADDS1 ? "ppc_ps_madds1"
                       : op == DOLIR_EXACT_PS_SUM0   ? "ppc_ps_sum0"
                                                     : "ppc_ps_sum1";
    auto callee = module_.getOrInsertFunction(
        name, FunctionType::get(Type::getVoidTy(context_),
                                {ptr, i8, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d), builder_.getInt8(a),
                                 builder_.getInt8(c), builder_.getInt8(b)});
  } else if (op == DOLIR_EXACT_PS_MULS0 || op == DOLIR_EXACT_PS_MULS1) {
    syncPair(a);
    syncPair(c);
    const char *name =
        op == DOLIR_EXACT_PS_MULS0 ? "ppc_ps_muls0" : "ppc_ps_muls1";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getVoidTy(context_), {ptr, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d), builder_.getInt8(a),
                                 builder_.getInt8(c)});
  } else {
    syncPair(a);
    u32 rhs = op == DOLIR_EXACT_PS_MUL ? c : b;
    syncPair(rhs);
    const char *name = op == DOLIR_EXACT_PS_ADD   ? "ppc_ps_add_op"
                       : op == DOLIR_EXACT_PS_SUB ? "ppc_ps_sub_op"
                       : op == DOLIR_EXACT_PS_MUL ? "ppc_ps_mul_op"
                                                  : "ppc_ps_div_op";
    auto callee = module_.getOrInsertFunction(
        name,
        FunctionType::get(Type::getVoidTy(context_), {ptr, i8, i8, i8}, false));
    builder_.CreateCall(callee, {ctx_, builder_.getInt8(d), builder_.getInt8(a),
                                 builder_.getInt8(rhs)});
  }
  reloadPair(d);
  reloadState(DOLIR_STATE_FPSCR);
}

Value *FunctionEmitter::emitPSQ(const DolIRInstruction &inst) {
  u32 reg = inst.immediate & 0xFFu;
  bool w = ((inst.immediate >> 8) & 1u) != 0;
  u32 gqr = (inst.immediate >> 9) & 7u;
  bool indexed = ((inst.immediate >> 12) & 1u) != 0;
  bool load = inst.aux == DOLIR_HELPER_PSQ_LOAD;
  auto fprSlot = [](u32 index) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_FPR0 + index);
  };
  auto ps1Slot = [](u32 index) {
    return static_cast<DolIRStateSlot>(DOLIR_STATE_PS1_0 + index);
  };
  Type *i32 = Type::getInt32Ty(context_);
  Type *f32 = Type::getFloatTy(context_);
  Type *f64 = Type::getDoubleTy(context_);
  Type *ptr = PointerType::getUnqual(context_);

  // Type 0 is the unquantised f32 form used throughout GXRuntime. Keep it in
  // SSA and use the normal generated memory path, avoiding the helper's full
  // CPUState materialise/reload boundary. Quantised formats and architecturally
  // exceptional cases retain the exact helper below.
  Value *gqrValue = stateValue(static_cast<DolIRStateSlot>(DOLIR_STATE_GQR0 + gqr));
  Value *type = load ? builder_.CreateLShr(gqrValue, builder_.getInt32(16))
                     : gqrValue;
  Value *typeZero = builder_.CreateICmpEQ(
      builder_.CreateAnd(type, builder_.getInt32(7)), builder_.getInt32(0));
  Value *hid2 = stateValue(DOLIR_STATE_HID2);
  u32 required = PPC_HID2_PSE | (indexed ? 0u : PPC_HID2_LSQE);
  Value *enabled = builder_.CreateICmpEQ(
      builder_.CreateAnd(hid2, builder_.getInt32(required)),
      builder_.getInt32(required));
  Value *address = operand(inst, 0);
  Value *aligned = builder_.CreateICmpEQ(
      builder_.CreateAnd(address, builder_.getInt32(3)), builder_.getInt32(0));
  Value *fastEligible =
      builder_.CreateAnd(typeZero, builder_.CreateAnd(enabled, aligned));

  BasicBlock *fast = BasicBlock::Create(context_, "psq_f32", function_);
  BasicBlock *slow = BasicBlock::Create(context_, "psq_exact", function_);
  BasicBlock *done = BasicBlock::Create(context_, "psq_done", function_);
  builder_.CreateCondBr(fastEligible, fast, slow);

  builder_.SetInsertPoint(fast);
  if (load) {
    Value *lane0Bits = emitGuestLoad(address, i32, 4, false);
    Value *lane0 = builder_.CreateFPExt(builder_.CreateBitCast(lane0Bits, f32), f64);
    builder_.CreateStore(lane0, state_[fprSlot(reg)]);
    if (w) {
      builder_.CreateStore(ConstantFP::get(f64, 1.0), state_[ps1Slot(reg)]);
    } else {
      Value *lane1Address = builder_.CreateAdd(address, builder_.getInt32(4));
      Value *lane1Bits = emitGuestLoad(lane1Address, i32, 4, false);
      Value *lane1 = builder_.CreateFPExt(builder_.CreateBitCast(lane1Bits, f32), f64);
      builder_.CreateStore(lane1, state_[ps1Slot(reg)]);
    }
  } else {
    const auto storeSingle = [&](Value *laneAddress, Value *lane) {
      Value *single = builder_.CreateFPTrunc(lane, f32);
      Value *bits = builder_.CreateBitCast(single, i32);
      Value *denormal = builder_.CreateAnd(
          builder_.CreateICmpEQ(
              builder_.CreateAnd(bits, builder_.getInt32(0x7F800000u)),
              builder_.getInt32(0)),
          builder_.CreateICmpNE(
              builder_.CreateAnd(bits, builder_.getInt32(0x007FFFFFu)),
              builder_.getInt32(0)));
      emitGuestStore(laneAddress,
                     builder_.CreateSelect(denormal, builder_.getInt32(0), bits),
                     4);
    };
    storeSingle(address, stateValue(fprSlot(reg)));
    if (!w) {
      Value *lane1Address = builder_.CreateAdd(address, builder_.getInt32(4));
      storeSingle(lane1Address, stateValue(ps1Slot(reg)));
    }
  }
  builder_.CreateBr(done);

  builder_.SetInsertPoint(slow);
  materialize(inst.guest_pc);
  auto callee = module_.getOrInsertFunction(
      load ? "ppc_psq_load" : "ppc_psq_store",
      FunctionType::get(Type::getInt1Ty(context_),
                        {ptr, Type::getInt8Ty(context_),
                         Type::getInt32Ty(context_), Type::getInt1Ty(context_),
                         Type::getInt8Ty(context_), Type::getInt1Ty(context_),
                         Type::getInt32Ty(context_)},
                        false));
  Value *success = builder_.CreateCall(
      callee, {ctx_, builder_.getInt8(reg), address,
               builder_.getInt1(w), builder_.getInt8(gqr),
               builder_.getInt1(indexed), builder_.getInt32(inst.guest_pc)});
  BasicBlock *resume = BasicBlock::Create(context_, "psq_exact_resume", function_);
  BasicBlock *failed = BasicBlock::Create(context_, "psq_exit", function_);
  builder_.CreateCondBr(success, resume, failed);
  builder_.SetInsertPoint(failed);
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(resume);
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (used_[slot])
      reloadState(static_cast<DolIRStateSlot>(slot));
  }
  builder_.CreateStore(builder_.getInt64(0), cycles_);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
  return ConstantInt::getTrue(context_);
}

void FunctionEmitter::emitStoreConditional(const DolIRInstruction &inst) {
  materialize(inst.guest_pc);
  Type *ptr = PointerType::getUnqual(context_);
  auto callee = module_.getOrInsertFunction(
      "ppc_stwcx_op", FunctionType::get(Type::getVoidTy(context_),
                                        {ptr, Type::getInt8Ty(context_),
                                         Type::getInt32Ty(context_),
                                         Type::getInt32Ty(context_)},
                                        false));
  builder_.CreateCall(callee,
                      {ctx_, builder_.getInt8(inst.immediate & 0xFFu),
                       operand(inst, 0), builder_.getInt32(inst.guest_pc)});
  Value *exception =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, exception));
  BasicBlock *resume = BasicBlock::Create(context_, "stwcx_resume", function_);
  BasicBlock *failed = BasicBlock::Create(context_, "stwcx_exit", function_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(exception, builder_.getInt32(0)),
                        resume, failed);
  builder_.SetInsertPoint(failed);
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(resume);
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (used_[slot])
      reloadState(static_cast<DolIRStateSlot>(slot));
  }
  builder_.CreateStore(builder_.getInt64(0), cycles_);
}

Value *FunctionEmitter::emitFPAvailable(u32 pc) {
  Value *msr =
      builder_.CreateLoad(Type::getInt32Ty(context_), state_[DOLIR_STATE_MSR]);
  Value *enabled = builder_.CreateICmpNE(
      builder_.CreateAnd(msr, builder_.getInt32(1u << 13)),
      builder_.getInt32(0));
  BasicBlock *fast = builder_.GetInsertBlock();
  BasicBlock *good = BasicBlock::Create(context_, "fp_ok", function_);
  BasicBlock *cold = BasicBlock::Create(context_, "fp_check", function_);
  builder_.CreateCondBr(enabled, good, cold);
  builder_.SetInsertPoint(cold);
  materialize(pc);
  auto callee = module_.getOrInsertFunction(
      "ppc_fp_available", FunctionType::get(Type::getInt1Ty(context_),
                                            {PointerType::getUnqual(context_),
                                             Type::getInt32Ty(context_)},
                                            false));
  Value *available = builder_.CreateCall(callee, {ctx_, builder_.getInt32(pc)});
  BasicBlock *reload = BasicBlock::Create(context_, "fp_reload", function_);
  BasicBlock *bad = BasicBlock::Create(context_, "fp_exit", function_);
  builder_.CreateCondBr(available, reload, bad);
  builder_.SetInsertPoint(bad);
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(reload);
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (used_[slot]) {
      auto stateSlot = static_cast<DolIRStateSlot>(slot);
      Value *reloaded = loadContext(stateSlot);
      builder_.CreateStore(reloaded, state_[slot]);
    }
  }
  builder_.CreateStore(builder_.getInt64(0), cycles_);
  builder_.CreateBr(good);
  builder_.SetInsertPoint(good);
  PHINode *checked = builder_.CreatePHI(Type::getInt1Ty(context_), 2);
  checked->addIncoming(ConstantInt::getTrue(context_), fast);
  checked->addIncoming(available, reload);
  return checked;
}

} // namespace dolllvm
