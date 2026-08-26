#include "backend/llvm/llvm_function_emitter.h"
#include "cpu/cpu.h"

#define DOLNATIVE_WITH_DOLIR 1
#include "core/dispatch_gate.h"
#include "core/native_state_layout.h"

#include <cstdio>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

namespace dolllvm {

using namespace llvm;

FunctionEmitter::FunctionEmitter(LLVMContext &context, Module &module,
                                 const DolIRFunction &source,
                                 const DolLLVMFunctionRange *ranges,
                                 u32 range_count, bool gamecube,
                                 StringRef symbol_prefix)
    : context_(context), module_(module), source_(source), builder_(context),
      ranges_(ranges), range_count_(range_count), gamecube_(gamecube),
      symbol_prefix_(symbol_prefix) {}

bool FunctionEmitter::emit(raw_ostream &diagnostics) {
  auto *pointer = PointerType::getUnqual(context_);
  auto *type = FunctionType::get(Type::getVoidTy(context_),
                                 {pointer, pointer, pointer}, false);
  const std::string wrapperName = symbolName(source_.name);
  const std::string bodyName = wrapperName + "_budget";
  function_ = module_.getFunction(bodyName);
  if (!function_)
    function_ = Function::Create(type, GlobalValue::ExternalLinkage, bodyName,
                                 module_);
  if (function_->getFunctionType() != type || !function_->empty()) {
    diagnostics << "dolllvm: conflicting native body " << bodyName << "\n";
    return false;
  }
  function_->setCallingConv(CallingConv::C);
  function_->setVisibility(GlobalValue::HiddenVisibility);
  function_->setDSOLocal(true);
  function_->addFnAttr(Attribute::NoInline);
  // Both counters are private wrapper-stack objects. They cannot overlap the
  // CPU state, emulated RAM reached through it, or one another. Describing
  // that fact lets LLVM keep their per-instruction increments in registers
  // instead of conservatively reloading around every guest memory access.
  for (unsigned index : {1u, 2u}) {
    function_->addParamAttr(index, Attribute::NoAlias);
    function_->addParamAttr(index, Attribute::NoCapture);
    function_->addParamAttr(index, Attribute::NonNull);
  }
  ctx_ = function_->getArg(0);
  ctx_->setName("ctx");
  guard_cycles_ = function_->getArg(1);
  guard_cycles_->setName("guard_cycles");
  guard_steps_ = function_->getArg(2);
  guard_steps_->setName("guard_steps");

  entry_ = BasicBlock::Create(context_, "entry", function_);
  for (u32 i = 0; i < source_.block_count; i++)
    blocks_.push_back(BasicBlock::Create(context_, blockName(i), function_));
  scanState();
  scanContinuations();
  scanLoopHeaders();
  emitEntry();
  for (u32 i = 0; i < source_.block_count; i++)
    if (!emitBlock(i, diagnostics))
      return false;
  if (verifyFunction(*function_, &diagnostics))
    return false;
  return emitWrapper(diagnostics);
}

bool FunctionEmitter::emitWrapper(raw_ostream &diagnostics) {
  auto *pointer = PointerType::getUnqual(context_);
  auto *type = FunctionType::get(Type::getVoidTy(context_), {pointer}, false);
  const std::string wrapperName = symbolName(source_.name);
  Function *wrapper = module_.getFunction(wrapperName);
  if (!wrapper)
    wrapper = Function::Create(type, GlobalValue::ExternalLinkage, wrapperName,
                               module_);
  if (wrapper->getFunctionType() != type || !wrapper->empty()) {
    diagnostics << "dolllvm: conflicting native entry " << wrapperName << "\n";
    return false;
  }
  wrapper->setCallingConv(CallingConv::C);
  wrapper->setVisibility(GlobalValue::HiddenVisibility);
  wrapper->setDSOLocal(true);
  wrapper->getArg(0)->setName("ctx");

  BasicBlock *entry = BasicBlock::Create(context_, "entry", wrapper);
  IRBuilder<> builder(entry);
  AllocaInst *guardCycles =
      builder.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guard_cycles");
  AllocaInst *guardSteps =
      builder.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guard_steps");
  builder.CreateStore(builder.getInt64(0), guardCycles);
  builder.CreateStore(builder.getInt64(0), guardSteps);
  builder.CreateCall(function_, {wrapper->getArg(0), guardCycles, guardSteps});
  builder.CreateRetVoid();
  return !verifyFunction(*wrapper, &diagnostics);
}

std::string FunctionEmitter::blockName(u32 index) const {
  char text[40];
  snprintf(text, sizeof(text), "guest_%08X_b%u",
           source_.blocks[index].guest_address, index);
  return text;
}

std::string FunctionEmitter::symbolName(StringRef name) const {
  return symbol_prefix_ + name.str();
}

Type *FunctionEmitter::type(DolIRType t) {
  switch (t) {
  case DOLIR_TYPE_I1:
    return Type::getInt1Ty(context_);
  case DOLIR_TYPE_I8:
    return Type::getInt8Ty(context_);
  case DOLIR_TYPE_I16:
    return Type::getInt16Ty(context_);
  case DOLIR_TYPE_I32:
    return Type::getInt32Ty(context_);
  case DOLIR_TYPE_I64:
    return Type::getInt64Ty(context_);
  case DOLIR_TYPE_F32:
    return Type::getFloatTy(context_);
  case DOLIR_TYPE_F64:
    return Type::getDoubleTy(context_);
  case DOLIR_TYPE_V2F32:
    return FixedVectorType::get(Type::getFloatTy(context_), 2);
  case DOLIR_TYPE_V2F64:
    return FixedVectorType::get(Type::getDoubleTy(context_), 2);
  default:
    return Type::getVoidTy(context_);
  }
}

size_t FunctionEmitter::stateOffset(DolIRStateSlot slot) const {
  return dolnative_state_offset(slot);
}

Value *FunctionEmitter::bytePtr(size_t offset) {
  return builder_.CreateInBoundsGEP(
      Type::getInt8Ty(context_), ctx_,
      ConstantInt::get(Type::getInt64Ty(context_), offset));
}

Value *FunctionEmitter::loadContext(DolIRStateSlot slot) {
  return builder_.CreateLoad(type(dolir_state_type(slot)),
                             bytePtr(stateOffset(slot)));
}

void FunctionEmitter::storeContext(DolIRStateSlot slot, Value *value) {
  builder_.CreateStore(value, bytePtr(stateOffset(slot)));
}

Value *FunctionEmitter::loadOffset(Type *valueType, size_t offset) {
  return builder_.CreateLoad(valueType, bytePtr(offset));
}

void FunctionEmitter::scanState() {
  for (u32 b = 0; b < source_.block_count; b++) {
    const DolIRBlock &block = source_.blocks[b];
    for (u32 i = 0; i < block.instruction_count; i++) {
      const DolIRInstruction &inst = block.instructions[i];
      if (inst.op == DOLIR_OP_STATE_READ || inst.op == DOLIR_OP_STATE_WRITE)
        used_[inst.aux] = true;
      if (inst.op == DOLIR_OP_STATE_WRITE)
        dirty_[inst.aux] = true;
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          inst.aux == DOLIR_HELPER_FP_AVAILABLE)
        used_[DOLIR_STATE_MSR] = true;
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          inst.aux == DOLIR_HELPER_EXACT_FLOAT)
        scanExactFloat(inst.immediate);
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          inst.aux == DOLIR_HELPER_EXACT_PAIRED)
        scanExactPaired(inst.immediate);
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          (inst.aux == DOLIR_HELPER_PSQ_LOAD ||
           inst.aux == DOLIR_HELPER_PSQ_STORE)) {
        u32 reg = inst.immediate & 0xFFu;
        u32 gqr = (inst.immediate >> 9) & 7u;
        used_[DOLIR_STATE_HID2] = true;
        used_[DOLIR_STATE_GQR0 + gqr] = true;
        used_[DOLIR_STATE_FPR0 + reg] = true;
        used_[DOLIR_STATE_PS1_0 + reg] = true;
        if (inst.aux == DOLIR_HELPER_PSQ_LOAD) {
          dirty_[DOLIR_STATE_FPR0 + reg] = true;
          dirty_[DOLIR_STATE_PS1_0 + reg] = true;
        } else {
          used_[DOLIR_STATE_RESERVE_ADDR] = true;
          used_[DOLIR_STATE_RESERVE_VALID] = true;
          dirty_[DOLIR_STATE_RESERVE_VALID] = true;
        }
      }
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          inst.aux == DOLIR_HELPER_STORE_CONDITIONAL) {
        used_[DOLIR_STATE_CR] = true;
        dirty_[DOLIR_STATE_CR] = true;
        used_[DOLIR_STATE_RESERVE_VALID] = true;
        dirty_[DOLIR_STATE_RESERVE_VALID] = true;
        used_[DOLIR_STATE_RESERVE_ADDR] = true;
      }
      if (inst.op == DOLIR_OP_HELPER_CALL &&
          (inst.aux == DOLIR_HELPER_FPSCR_UPDATED ||
           inst.aux == DOLIR_HELPER_FPSCR_BIT)) {
        used_[DOLIR_STATE_FPSCR] = true;
        dirty_[DOLIR_STATE_FPSCR] = true;
      }
      if (inst.op == DOLIR_OP_HELPER_CALL && inst.aux == DOLIR_HELPER_LSWX) {
        used_[DOLIR_STATE_XER] = true;
        for (u32 reg = 0; reg < 32; reg++) {
          used_[DOLIR_STATE_GPR0 + reg] = true;
          dirty_[DOLIR_STATE_GPR0 + reg] = true;
        }
      }
      if (inst.op == DOLIR_OP_GUEST_STORE) {
        used_[DOLIR_STATE_RESERVE_ADDR] = true;
        used_[DOLIR_STATE_RESERVE_VALID] = true;
        dirty_[DOLIR_STATE_RESERVE_VALID] = true;
      }
    }
  }
}

void FunctionEmitter::scanExactFloat(u64 descriptor) {
  auto op = static_cast<DolIRExactFloat>(descriptor & 0xFFu);
  u32 d = (descriptor >> 8) & 0xFFu;
  u32 a = (descriptor >> 16) & 0xFFu;
  u32 b = (descriptor >> 24) & 0xFFu;
  u32 c = (descriptor >> 32) & 0xFFu;
  used_[DOLIR_STATE_FPSCR] = true;
  dirty_[DOLIR_STATE_FPSCR] = true;
  if (op == DOLIR_EXACT_FCMPU || op == DOLIR_EXACT_FCMPO) {
    used_[DOLIR_STATE_CR] = true;
    dirty_[DOLIR_STATE_CR] = true;
    used_[DOLIR_STATE_FPR0 + a] = true;
    used_[DOLIR_STATE_FPR0 + b] = true;
    return;
  }
  used_[DOLIR_STATE_FPR0 + d] = true;
  dirty_[DOLIR_STATE_FPR0 + d] = true;
  if (op == DOLIR_EXACT_FRES ||
      (op >= DOLIR_EXACT_FADDS && op <= DOLIR_EXACT_FDIVS) ||
      op == DOLIR_EXACT_FRSP ||
      (op >= DOLIR_EXACT_FMADDS && op <= DOLIR_EXACT_FNMSUBS)) {
    used_[DOLIR_STATE_PS1_0 + d] = true;
    dirty_[DOLIR_STATE_PS1_0 + d] = true;
  }
  if (op == DOLIR_EXACT_FRES || op == DOLIR_EXACT_FRSQRTE ||
      op == DOLIR_EXACT_FCTIW || op == DOLIR_EXACT_FCTIWZ ||
      op == DOLIR_EXACT_FRSP) {
    used_[DOLIR_STATE_FPR0 + b] = true;
  } else if (op == DOLIR_EXACT_FMULS || op == DOLIR_EXACT_FMUL) {
    used_[DOLIR_STATE_FPR0 + a] = true;
    used_[DOLIR_STATE_FPR0 + c] = true;
  } else if ((op >= DOLIR_EXACT_FADDS && op <= DOLIR_EXACT_FDIVS) ||
             (op >= DOLIR_EXACT_FADD && op <= DOLIR_EXACT_FDIV)) {
    used_[DOLIR_STATE_FPR0 + a] = true;
    used_[DOLIR_STATE_FPR0 + b] = true;
  } else {
    used_[DOLIR_STATE_FPR0 + a] = true;
    used_[DOLIR_STATE_FPR0 + b] = true;
    used_[DOLIR_STATE_FPR0 + c] = true;
  }
}

void FunctionEmitter::scanExactPaired(u64 descriptor) {
  auto op = static_cast<DolIRExactPaired>(descriptor & 0xFFu);
  u32 d = (descriptor >> 8) & 0xFFu;
  u32 a = (descriptor >> 16) & 0xFFu;
  u32 b = (descriptor >> 24) & 0xFFu;
  u32 c = (descriptor >> 32) & 0xFFu;
  used_[DOLIR_STATE_FPSCR] = true;
  dirty_[DOLIR_STATE_FPSCR] = true;
  if (op >= DOLIR_EXACT_PS_CMPU0) {
    used_[DOLIR_STATE_CR] = true;
    dirty_[DOLIR_STATE_CR] = true;
    used_[DOLIR_STATE_FPR0 + a] = true;
    used_[DOLIR_STATE_PS1_0 + a] = true;
    used_[DOLIR_STATE_FPR0 + b] = true;
    used_[DOLIR_STATE_PS1_0 + b] = true;
    return;
  }
  used_[DOLIR_STATE_FPR0 + d] = true;
  dirty_[DOLIR_STATE_FPR0 + d] = true;
  used_[DOLIR_STATE_PS1_0 + d] = true;
  dirty_[DOLIR_STATE_PS1_0 + d] = true;
  auto usePair = [this](u32 reg) {
    used_[DOLIR_STATE_FPR0 + reg] = true;
    used_[DOLIR_STATE_PS1_0 + reg] = true;
  };
  if (op == DOLIR_EXACT_PS_RES || op == DOLIR_EXACT_PS_RSQRTE) {
    usePair(b);
    return;
  }
  usePair(a);
  if (op == DOLIR_EXACT_PS_MUL || op == DOLIR_EXACT_PS_MULS0 ||
      op == DOLIR_EXACT_PS_MULS1) {
    usePair(c);
    return;
  }
  usePair(b);
  if (op >= DOLIR_EXACT_PS_MADD && op <= DOLIR_EXACT_PS_SUM1)
    usePair(c);
}

void FunctionEmitter::scanContinuations() {
  for (u32 i = 0; i < source_.block_count; i++) {
    const DolIRTerminator &term = source_.blocks[i].terminator;
    if (!term.linked)
      continue;
    u32 continuation = term.guest_pc + 4u;
    u32 block = 0;
    if (continuation >= source_.guest_start &&
        continuation < source_.guest_end &&
        ((continuation - source_.guest_start) & 3u) == 0) {
      block = (continuation - source_.guest_start) / 4u;
      if (block < source_.block_count)
        continuations_.push_back(block);
    }
  }
}

void FunctionEmitter::scanLoopHeaders() {
  loop_headers_.assign(source_.block_count, false);
  for (u32 i = 0; i < source_.block_count; i++) {
    const DolIRTerminator &term = source_.blocks[i].terminator;
    u32 count = term.kind == DOLIR_TERM_COND_BRANCH ? 2u
                : term.kind == DOLIR_TERM_BRANCH    ? 1u
                                                    : 0u;
    for (u32 edge = 0; edge < count; edge++) {
      if (term.targets[edge] != DOLIR_NO_BLOCK && term.targets[edge] <= i)
        loop_headers_[term.targets[edge]] = true;
    }
  }
}

void FunctionEmitter::emitEntry() {
  builder_.SetInsertPoint(entry_);
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (!used_[slot])
      continue;
    auto stateSlot = static_cast<DolIRStateSlot>(slot);
    state_[slot] = builder_.CreateAlloca(type(dolir_state_type(stateSlot)),
                                         nullptr, "state");
    builder_.CreateStore(loadContext(stateSlot), state_[slot]);
  }
  cycles_ =
      builder_.CreateAlloca(Type::getInt64Ty(context_), nullptr, "cycles");
  builder_.CreateStore(ConstantInt::get(Type::getInt64Ty(context_), 0),
                       cycles_);
  // The chassis cannot replace a running core's RAM mappings while a native
  // body is executing. Keep one SSA snapshot so calls that legitimately
  // mutate architectural CPUState do not pessimize every memory operation.
  Type *pointer = PointerType::getUnqual(context_);
  ram_ = loadOffset(pointer, offsetof(CPUState, ram));
  ram_size_ =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, ram_size));
  if (gamecube_) {
    exram_ = ConstantPointerNull::get(cast<PointerType>(pointer));
    exram_size_ = builder_.getInt32(0);
  } else {
    exram_ = loadOffset(pointer, offsetof(CPUState, exram));
    exram_size_ =
        loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, exram_size));
  }
  const std::string gateName = symbolName("dolrecomp_native_gate");
  GlobalVariable *gate = module_.getGlobalVariable(gateName, true);
  if (!gate) {
    gate = new GlobalVariable(
        module_, ArrayType::get(Type::getInt8Ty(context_),
                                sizeof(StaticRecompDispatchGate)),
        false, GlobalValue::ExternalLinkage, nullptr,
        gateName);
    gate->setVisibility(GlobalValue::HiddenVisibility);
    gate->setDSOLocal(true);
  }
  const auto gateField = [&](Type *fieldType, size_t offset) {
    Value *field = builder_.CreateInBoundsGEP(
        Type::getInt8Ty(context_), gate, builder_.getInt64(offset));
    return builder_.CreateLoad(fieldType, field);
  };
  gate_chunk_open_ = gateField(
      pointer, offsetof(StaticRecompDispatchGate, chunk_open));
  gate_budget_ =
      gateField(pointer, offsetof(StaticRecompDispatchGate, budget));
  gate_pending_ =
      gateField(pointer, offsetof(StaticRecompDispatchGate, pending));
  gate_pending_sync_ = gateField(
      Type::getInt32Ty(context_),
      offsetof(StaticRecompDispatchGate, pending_sync));
  gate_pending_async_ = gateField(
      Type::getInt32Ty(context_),
      offsetof(StaticRecompDispatchGate, pending_async));
  Value *pc = loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, pc));
  BasicBlock *bad = BasicBlock::Create(context_, "entry_miss", function_);
  auto *dispatch = builder_.CreateSwitch(pc, bad, source_.block_count);
  for (u32 i = 0; i < source_.block_count; i++)
    dispatch->addCase(ConstantInt::get(Type::getInt32Ty(context_),
                                       source_.blocks[i].guest_address),
                      blocks_[i]);
  builder_.SetInsertPoint(bad);
  builder_.CreateRetVoid();
}

void FunctionEmitter::chargeCycles(u32 cycles) {
  Value *old = builder_.CreateLoad(Type::getInt64Ty(context_), cycles_);
  Value *next = builder_.CreateAdd(
      old, ConstantInt::get(Type::getInt64Ty(context_), cycles));
  builder_.CreateStore(next, cycles_);
  // The shared guard survives helper and generated-function boundaries.
  Value *guard_old =
      builder_.CreateLoad(Type::getInt64Ty(context_), guard_cycles_);
  builder_.CreateStore(
      builder_.CreateAdd(guard_old,
                         ConstantInt::get(Type::getInt64Ty(context_), cycles)),
      guard_cycles_);
}

void FunctionEmitter::materialize(u32 pc) {
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    if (!dirty_[slot])
      continue;
    auto stateSlot = static_cast<DolIRStateSlot>(slot);
    storeContext(
        stateSlot,
        builder_.CreateLoad(type(dolir_state_type(stateSlot)), state_[slot]));
  }
  storeContext(DOLIR_STATE_PC,
               ConstantInt::get(Type::getInt32Ty(context_), pc));
  Value *downcount =
      loadOffset(Type::getInt64Ty(context_), offsetof(CPUState, downcount));
  // Only unmaterialized cycles are owed to downcount.
  Value *cycles = builder_.CreateLoad(Type::getInt64Ty(context_), cycles_);
  builder_.CreateStore(builder_.CreateSub(downcount, cycles),
                       bytePtr(offsetof(CPUState, downcount)));
}

void FunctionEmitter::sideExit(u32 pc) {
  materialize(pc);
  builder_.CreateRetVoid();
}

void FunctionEmitter::emitBudgetGuard(u32 pc) {
  Type *i8 = Type::getInt8Ty(context_);
  Type *i32 = Type::getInt32Ty(context_);
  Type *i64 = Type::getInt64Ty(context_);

  // Guard the whole native call chain, not one generated function.  Without a
  // published chassis gate (host execution tests and standalone modules), keep
  // the conservative fixed allowance.  An attached iOS module instead runs to
  // the end of the chassis's live slice, exactly as DolVM does.
  Value *hasGate = builder_.CreateAnd(
      builder_.CreateIsNotNull(gate_chunk_open_),
      builder_.CreateAnd(builder_.CreateIsNotNull(gate_budget_),
                         builder_.CreateIsNotNull(gate_pending_)));

  BasicBlock *fixed = BasicBlock::Create(context_, "budget_fixed", function_);
  BasicBlock *live = BasicBlock::Create(context_, "budget_live", function_);
  BasicBlock *checked = BasicBlock::Create(context_, "budget_checked", function_);
  builder_.CreateCondBr(hasGate, live, fixed);

  builder_.SetInsertPoint(fixed);
  Value *fixedCycles = builder_.CreateLoad(i64, guard_cycles_);
  Value *fixedOverCycles = builder_.CreateICmpUGE(
      fixedCycles, ConstantInt::get(i64, 256));
  Value *fixedSteps = builder_.CreateLoad(i64, guard_steps_);
  Value *fixedNextSteps = builder_.CreateAdd(fixedSteps, ConstantInt::get(i64, 1));
  builder_.CreateStore(fixedNextSteps, guard_steps_);
  Value *fixedOverSteps = builder_.CreateICmpUGE(
      fixedNextSteps, ConstantInt::get(i64, 2048));
  Value *fixedExhausted = builder_.CreateOr(fixedOverCycles, fixedOverSteps);
  builder_.CreateBr(checked);

  builder_.SetInsertPoint(live);
  Value *budget32 = builder_.CreateLoad(i32, gate_budget_);
  Value *budgetPositive = builder_.CreateICmpSGT(budget32, builder_.getInt32(0));
  Value *budget64 = builder_.CreateZExt(budget32, i64);
  // Hooks flush materialized charge and reset ctx->downcount.  Add only the
  // charge not materialized yet; a lifetime counter would double-count every
  // hook, the same failure mode the cross-chunk gate explicitly avoids.
  Value *materialized = loadOffset(i64, offsetof(CPUState, downcount));
  Value *unmaterialized = builder_.CreateLoad(i64, cycles_);
  Value *unflushed = builder_.CreateAdd(builder_.CreateNeg(materialized),
                                        unmaterialized);
  Value *liveOverCycles = builder_.CreateOr(
      builder_.CreateNot(budgetPositive),
      builder_.CreateICmpUGE(unflushed, budget64));

  Value *liveSteps = builder_.CreateLoad(i64, guard_steps_);
  Value *liveNextSteps = builder_.CreateAdd(liveSteps, ConstantInt::get(i64, 1));
  builder_.CreateStore(liveNextSteps, guard_steps_);
  Value *stepLimit = builder_.CreateSelect(
      builder_.CreateICmpUGT(budget64, ConstantInt::get(i64, 2048)),
      budget64, ConstantInt::get(i64, 2048));
  Value *liveOverSteps = builder_.CreateICmpUGE(liveNextSteps, stepLimit);

  const DolLLVMFunctionRange *currentRange = rangeFor(source_.guest_start);
  u32 currentIndex = 0;
  if (currentRange) {
    for (u32 i = 0; i < range_count_; ++i)
      if (ranges_[i].start < currentRange->start)
        ++currentIndex;
  }
  Value *currentOpen = builder_.CreateLoad(
      i8, builder_.CreateInBoundsGEP(i8, gate_chunk_open_,
                                     builder_.getInt64(currentIndex)));
  Value *chunkClosed = builder_.CreateICmpEQ(currentOpen, builder_.getInt8(0));

  Value *pending = builder_.CreateLoad(i32, gate_pending_);
  Value *syncPending = builder_.CreateICmpNE(
      builder_.CreateAnd(pending, gate_pending_sync_), builder_.getInt32(0));
  Value *msr = used_[DOLIR_STATE_MSR] ? stateValue(DOLIR_STATE_MSR)
                                      : loadContext(DOLIR_STATE_MSR);
  Value *interruptsEnabled = builder_.CreateICmpNE(
      builder_.CreateAnd(msr, builder_.getInt32(0x00008000u)),
      builder_.getInt32(0));
  Value *asyncPending = builder_.CreateAnd(
      interruptsEnabled,
      builder_.CreateICmpNE(builder_.CreateAnd(pending, gate_pending_async_),
                            builder_.getInt32(0)));
  Value *liveExhausted = builder_.CreateOr(
      builder_.CreateOr(liveOverCycles, liveOverSteps),
      builder_.CreateOr(chunkClosed,
                        builder_.CreateOr(syncPending, asyncPending)));
  builder_.CreateBr(checked);

  builder_.SetInsertPoint(checked);
  PHINode *exhausted = builder_.CreatePHI(Type::getInt1Ty(context_), 2);
  exhausted->addIncoming(fixedExhausted, fixed);
  exhausted->addIncoming(liveExhausted, live);
  BasicBlock *run = BasicBlock::Create(context_, "budget_run", function_);
  BasicBlock *exit = BasicBlock::Create(context_, "budget_exit", function_);
  builder_.CreateCondBr(exhausted, exit, run);
  builder_.SetInsertPoint(exit);
  sideExit(pc);
  builder_.SetInsertPoint(run);
}

bool FunctionEmitter::emitBlock(u32 index, raw_ostream &diagnostics) {
  const DolIRBlock &block = source_.blocks[index];
  builder_.SetInsertPoint(blocks_[index]);
  if (loop_headers_[index])
    emitBudgetGuard(block.guest_address);
  chargeCycles(block.cycle_cost);
  values_.assign(source_.value_count, nullptr);
  for (u32 i = 0; i < block.instruction_count; i++) {
    if (!emitInstruction(block.instructions[i], diagnostics))
      return false;
  }
  return emitTerminator(block.terminator, diagnostics);
}

Value *FunctionEmitter::operand(const DolIRInstruction &inst, u32 index) {
  return values_[inst.operands[index]];
}

Value *FunctionEmitter::castValue(DolIROp op, Type *resultType, Value *value) {
  switch (op) {
  case DOLIR_OP_TRUNC:
    return builder_.CreateTrunc(value, resultType);
  case DOLIR_OP_ZEXT:
    return builder_.CreateZExt(value, resultType);
  case DOLIR_OP_SEXT:
    return builder_.CreateSExt(value, resultType);
  case DOLIR_OP_BITCAST:
    return builder_.CreateBitCast(value, resultType);
  case DOLIR_OP_FPTRUNC:
    return builder_.CreateFPTrunc(value, resultType);
  case DOLIR_OP_FPEXT:
    return builder_.CreateFPExt(value, resultType);
  default:
    return nullptr;
  }
}

Value *FunctionEmitter::bswap(Value *value) {
  auto *integer = cast<IntegerType>(value->getType());
  if (integer->getBitWidth() == 8)
    return value;
  FunctionCallee intrinsic = Intrinsic::getOrInsertDeclaration(
      &module_, Intrinsic::bswap, {value->getType()});
  return builder_.CreateCall(intrinsic, {value});
}

bool FunctionEmitter::emitInstruction(const DolIRInstruction &inst,
                                      raw_ostream &diagnostics) {
  current_pc_ = inst.guest_pc;
  Value *result = nullptr;
  Type *resultType = type(inst.type);
  switch (inst.op) {
  case DOLIR_OP_CONSTANT:
    if (inst.type == DOLIR_TYPE_F32)
      result = ConstantFP::get(
          context_, APFloat(APFloat::IEEEsingle(), APInt(32, inst.immediate)));
    else if (inst.type == DOLIR_TYPE_F64)
      result = ConstantFP::get(
          context_, APFloat(APFloat::IEEEdouble(), APInt(64, inst.immediate)));
    else
      result = ConstantInt::get(resultType, inst.immediate);
    break;
  case DOLIR_OP_STATE_READ:
    result = builder_.CreateLoad(resultType, state_[inst.aux]);
    break;
  case DOLIR_OP_STATE_WRITE:
    builder_.CreateStore(operand(inst, 0), state_[inst.aux]);
    break;
  case DOLIR_OP_ADD:
    result = builder_.CreateAdd(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_SUB:
    result = builder_.CreateSub(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_MUL:
    result = builder_.CreateMul(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_UDIV:
    result = builder_.CreateUDiv(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_SDIV:
    result = builder_.CreateSDiv(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_AND:
    result = builder_.CreateAnd(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_OR:
    result = builder_.CreateOr(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_XOR:
    result = builder_.CreateXor(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_NOT:
    result = builder_.CreateNot(operand(inst, 0));
    break;
  case DOLIR_OP_SHL:
    result = builder_.CreateShl(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_LSHR:
    result = builder_.CreateLShr(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ASHR:
    result = builder_.CreateAShr(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ROTL: {
    FunctionCallee intrinsic = Intrinsic::getOrInsertDeclaration(
        &module_, Intrinsic::fshl, {resultType});
    result = builder_.CreateCall(
        intrinsic, {operand(inst, 0), operand(inst, 0), operand(inst, 1)});
    break;
  }
  case DOLIR_OP_CLZ: {
    FunctionCallee intrinsic = Intrinsic::getOrInsertDeclaration(
        &module_, Intrinsic::ctlz, {resultType});
    result = builder_.CreateCall(
        intrinsic, {operand(inst, 0), ConstantInt::getFalse(context_)});
    break;
  }
  case DOLIR_OP_BSWAP:
    result = bswap(operand(inst, 0));
    break;
  case DOLIR_OP_TRUNC:
  case DOLIR_OP_ZEXT:
  case DOLIR_OP_SEXT:
  case DOLIR_OP_BITCAST:
  case DOLIR_OP_FPTRUNC:
  case DOLIR_OP_FPEXT:
    result = castValue(inst.op, resultType, operand(inst, 0));
    break;
  case DOLIR_OP_ICMP_EQ:
    result = builder_.CreateICmpEQ(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ICMP_NE:
    result = builder_.CreateICmpNE(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ICMP_ULT:
    result = builder_.CreateICmpULT(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ICMP_ULE:
    result = builder_.CreateICmpULE(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ICMP_SLT:
    result = builder_.CreateICmpSLT(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_ICMP_SLE:
    result = builder_.CreateICmpSLE(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FCMP_OEQ:
    result = builder_.CreateFCmpOEQ(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FCMP_OLT:
    result = builder_.CreateFCmpOLT(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FCMP_OGE:
    result = builder_.CreateFCmpOGE(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_SELECT:
    result = builder_.CreateSelect(operand(inst, 0), operand(inst, 1),
                                   operand(inst, 2));
    break;
  case DOLIR_OP_FADD:
    result = builder_.CreateFAdd(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FSUB:
    result = builder_.CreateFSub(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FMUL:
    result = builder_.CreateFMul(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FDIV:
    result = builder_.CreateFDiv(operand(inst, 0), operand(inst, 1));
    break;
  case DOLIR_OP_FNEG:
    result = builder_.CreateFNeg(operand(inst, 0));
    break;
  case DOLIR_OP_FABS: {
    FunctionCallee intrinsic = Intrinsic::getOrInsertDeclaration(
        &module_, Intrinsic::fabs, {resultType});
    result = builder_.CreateCall(intrinsic, {operand(inst, 0)});
    break;
  }
  case DOLIR_OP_VECTOR_BUILD: {
    result = PoisonValue::get(resultType);
    result =
        builder_.CreateInsertElement(result, operand(inst, 0), uint64_t{0});
    result = builder_.CreateInsertElement(result, operand(inst, 1), 1u);
    break;
  }
  case DOLIR_OP_VECTOR_EXTRACT:
    result = builder_.CreateExtractElement(operand(inst, 0), inst.aux);
    break;
  case DOLIR_OP_VECTOR_SHUFFLE:
    result = builder_.CreateShuffleVector(
        operand(inst, 0), operand(inst, 1),
        {static_cast<int>(inst.aux & 0xFFu),
         static_cast<int>((inst.aux >> 8) & 0xFFu)});
    break;
  case DOLIR_OP_GUEST_LOAD:
    result = emitGuestLoad(operand(inst, 0), resultType, inst.aux & 0xffu,
                           (inst.aux & 0x100u) != 0);
    break;
  case DOLIR_OP_GUEST_STORE:
    emitGuestStore(operand(inst, 0), operand(inst, 1), inst.aux & 0xffu);
    break;
  case DOLIR_OP_HELPER_CALL:
    if (inst.aux == DOLIR_HELPER_FP_AVAILABLE)
      result = emitFPAvailable(inst.guest_pc);
    else if (inst.aux == DOLIR_HELPER_MEMORY_FENCE)
      builder_.CreateFence(AtomicOrdering::SequentiallyConsistent);
    else if (inst.aux == DOLIR_HELPER_EXACT_FLOAT)
      emitExactFloat(inst.immediate);
    else if (inst.aux == DOLIR_HELPER_EXACT_PAIRED)
      emitExactPaired(inst.immediate);
    else if (inst.aux == DOLIR_HELPER_PSQ_LOAD ||
             inst.aux == DOLIR_HELPER_PSQ_STORE)
      result = emitPSQ(inst);
    else if (inst.aux == DOLIR_HELPER_STORE_CONDITIONAL)
      emitStoreConditional(inst);
    else if (inst.aux == DOLIR_HELPER_FPSCR_UPDATED)
      emitFPSCRUpdated();
    else if (inst.aux == DOLIR_HELPER_FPSCR_BIT)
      emitFPSCRBit(inst.immediate);
    else if (inst.aux == DOLIR_HELPER_PROGRAM_EXCEPTION)
      emitProgramException(inst);
    else if (inst.aux == DOLIR_HELPER_SPR_READ)
      result = emitSPRRead(inst);
    else if (inst.aux == DOLIR_HELPER_SPR_WRITE)
      emitSPRWrite(inst);
    else if (inst.aux == DOLIR_HELPER_LSWX)
      emitLSWX(inst);
    else if (inst.aux == DOLIR_HELPER_DCBZ_L ||
             inst.aux == DOLIR_HELPER_ECIWX || inst.aux == DOLIR_HELPER_ECOWX ||
             inst.aux == DOLIR_HELPER_TLBIE ||
             inst.aux == DOLIR_HELPER_CACHE_CONTROL)
      result = emitRuntimeBoundary(inst);
    else {
      diagnostics << "dolllvm: unsupported helper " << inst.aux << " at 0x"
                  << format_hex_no_prefix(inst.guest_pc, 8) << "\n";
      return false;
    }
    break;
  default:
    diagnostics << "dolllvm: unsupported DolIR op " << unsigned(inst.op)
                << " at 0x" << format_hex_no_prefix(inst.guest_pc, 8) << "\n";
    return false;
  }
  if (inst.result)
    values_[inst.result] = result;
  return inst.type == DOLIR_TYPE_VOID || result != nullptr;
}

} // namespace dolllvm
