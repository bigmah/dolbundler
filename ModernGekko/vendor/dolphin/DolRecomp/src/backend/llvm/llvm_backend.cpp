#include "backend/llvm/llvm_backend.h"
#include "backend/llvm/llvm_function_emitter.h"
#include "cpu/cpu.h"

#define DOLNATIVE_WITH_DOLIR 1
#include "core/native_state_layout.h"

#include "backend/llvm/llvm_target_layout.h"

#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Instrumentation/InstrProfiling.h>
#include <llvm/Transforms/Instrumentation/PGOInstrumentation.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Config/llvm-config.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

using namespace llvm;

// Shared by emission, cache hashing and resume validation.
static std::string resolveTriple(const char *requested) {
  const std::string raw = requested && requested[0]
                              ? std::string(requested)
                              : llvm::sys::getDefaultTargetTriple();
  return llvm::Triple::normalize(raw);
}

static bool supportedTarget(const llvm::Triple &triple) {
  // wasm32 is a 32-bit target, so it does not share the host's CPUState layout
  // the way every other target here does. Nothing below assumes it does any
  // more -- see targetStateLayout() -- and module-template's static asserts
  // check the result when they are compiled for wasm32, so a mistake in the
  // layout is a build failure rather than a silently wrong module.
  if (triple.getArch() == llvm::Triple::wasm32)
    return true;
  if (triple.getArch() == llvm::Triple::x86_64)
    return triple.isOSLinux() || triple.isOSWindows();
  if (triple.getArch() != llvm::Triple::aarch64 ||
      triple.getArchName() == "arm64e")
    return false;
  if (triple.isiOS() && !triple.isTvOS())
    // Device and Apple-silicon simulator builds use the same generated IR and
    // AArch64 ABI but distinct Mach-O platforms. Supporting both gives native
    // modules a fast, symbolicated development loop while matchesMachO() below
    // still prevents either object kind from entering the other app.
    return !triple.getOSVersion().empty();
  const bool isMacOS = triple.isMacOSX() ||
                       triple.getOS() == llvm::Triple::Darwin;
  return isMacOS && !triple.isSimulatorEnvironment();
}

// Everything below feeds both codegen and the object cache key. Changing any of
// it must invalidate cached objects, so keep it reachable from one place: the
// fingerprint is built from these same accessors, and an edit that misses the
// key produces a build that silently reuses objects from the old settings and
// reports them as a result.
//
// DOLRECOMP_LLVM_CPU and DOLRECOMP_LLVM_FEATURES override the target machine's
// CPU and feature string; "native" resolves to the host. The default stays
// "generic" with no features, which is the portable x86-64 baseline -- a
// released module must run on any x86-64 host, so raising the target has to
// stay opt-in.
//
// That baseline already includes SSE2, which is not incidental here: E001
// measured the vectorizers as worth 1.37x precisely because Gekko
// paired-singles are 2-wide f32 pairs that map onto SSE2. So a wider target is
// a live hypothesis rather than a long shot.
static constexpr const char *kDefaultTargetFeatures = "";

static std::string targetCPU(const llvm::Triple &triple) {
  const char *cpu = getenv("DOLRECOMP_LLVM_CPU");
  if (!cpu || !cpu[0])
    return triple.isiOS() ? "apple-a16" : "generic";
  // createTargetMachine does not expand "native" itself.
  if (!strcmp(cpu, "native"))
    return llvm::sys::getHostCPUName().str();
  return cpu;
}

// wasm objects carry the feature set they were built with, and wasm-ld refuses
// to give shared memory to a module any of whose objects lacks atomics and
// bulk-memory: "--shared-memory is disallowed by chunk_....o because it was not
// compiled with 'atomics' or 'bulk-memory' features". The emulator is threaded,
// so every object has to agree with the rest of the build. This is the same
// trap build.sh documents for the C backend, where -pthread has to reach every
// translation unit and not just the link.
static constexpr const char *kWasmTargetFeatures =
    "+atomics,+bulk-memory,+mutable-globals,+sign-ext";

static std::string targetFeatures(const llvm::Triple &triple) {
  const char *features = getenv("DOLRECOMP_LLVM_FEATURES");
  if (features)
    return features;
  if (triple.isWasm())
    return kWasmTargetFeatures;
  return kDefaultTargetFeatures;
}

// instcombine's fixpoint check is a self-diagnostic for the pass, not a
// correctness property of the IR. Recompiled Gekko functions contain long
// straight-line integer and condition-flag sequences that can still be changing
// after one iteration, which makes the pass call report_fatal_error and take the
// whole recompilation down. Suppressing the check leaves the optimization
// itself intact.
//
// Do not remove the vectorizers. Measured (LLVM-EXPERIMENTS E001): dropping
// loop-vectorize, slp-vectorizer and vector-combine costs **-27%** throughput
// and makes the module 4.6% *larger*. The intuition that they cannot pay off
// against CPU "generic" with an empty feature string is wrong -- SSE2 is part
// of the x86-64 baseline, and Gekko paired-singles are inherently 2-wide f32
// pairs, so SLP has real work to do at that baseline.
static constexpr const char *kPassPipeline =
    "function(mem2reg,early-cse<memssa>,instcombine<no-verify-fixpoint>,"
    "simplifycfg,sccp,"
    "correlated-propagation,jump-threading,gvn,dse,adce,loop-simplify,"
    "loop-rotate,loop-mssa(licm),loop-vectorize,slp-vectorizer,vector-"
    "combine,"
    "tailcallelim),cgscc(inline),ipsccp,globaldce";

// DOLRECOMP_LLVM_PIPELINE=size drops the three vectorisers and the inliner.
// A whole game is tens of megabytes of generated code and the wasm module is
// most of the binary, so on a target where the module has to be downloaded and
// held in a phone's memory, code size is a first-class cost rather than a
// rounding error -- and vectorising for a target built without SIMD is paying
// that cost for nothing.
static constexpr const char *kSizePassPipeline =
    "function(mem2reg,early-cse<memssa>,instcombine<no-verify-fixpoint>,"
    "simplifycfg,sccp,"
    "correlated-propagation,jump-threading,gvn,dse,adce,loop-simplify,"
    "loop-rotate,loop-mssa(licm),"
    "tailcallelim),ipsccp,globaldce";

static const char *passPipeline() {
  const char *choice = getenv("DOLRECOMP_LLVM_PIPELINE");
  if (choice && std::string(choice) == "size")
    return kSizePassPipeline;
  return kPassPipeline;
}

// The profile path, and a content hash of it, resolved once. The hash is what
// goes in the cache key: two different profiles written to the same path must
// not share objects, and a profile regenerated in place by a collection script
// makes the path alone a non-identity -- exactly the silent stale-reuse the
// fingerprint exists to prevent.
static const std::string &pgoProfilePath() {
  static const std::string path = [] {
    const char *file = getenv("DOLRECOMP_LLVM_PROFILE");
    return std::string(file ? file : "");
  }();
  return path;
}

static const std::string &pgoProfileFingerprint() {
  static const std::string fingerprint = [] {
    if (pgoProfilePath().empty())
      return std::string("none");
    FILE *file = fopen(pgoProfilePath().c_str(), "rb");
    if (!file)
      return std::string("missing");
    unsigned long long hash = 1469598103934665603ull;
    unsigned long long size = 0;
    unsigned char buffer[65536];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
      size += read;
      for (size_t i = 0; i < read; i++) {
        hash ^= buffer[i];
        hash *= 1099511628211ull;
      }
    }
    fclose(file);
    char text[64];
    snprintf(text, sizeof(text), "%016llx/%llu", hash, size);
    return std::string(text);
  }();
  return fingerprint;
}

// Off by default, so the default module stays byte-identical to an unprofiled
// build and the existing object cache keeps its meaning. "use" without a
// readable DOLRECOMP_LLVM_PROFILE is refused rather than silently degraded to
// an unprofiled build -- an untrained PGO build that looks trained is the one
// failure mode that corrupts a measurement instead of stopping it.
extern "C" int dolllvm_pgo_mode(void) {
  static const int mode = [] {
    const char *requested = getenv("DOLRECOMP_LLVM_PGO");
    if (!requested || !requested[0] || !strcmp(requested, "0") ||
        !strcmp(requested, "off"))
      return (int)DOLLLVM_PGO_OFF;
    if (!strcmp(requested, "gen"))
      return (int)DOLLLVM_PGO_GEN;
    if (strcmp(requested, "use")) {
      fprintf(stderr, "dolllvm: DOLRECOMP_LLVM_PGO must be gen, use or off\n");
      abort();
    }
    if (pgoProfilePath().empty() || pgoProfileFingerprint() == "missing") {
      fprintf(stderr,
              "dolllvm: DOLRECOMP_LLVM_PGO=use needs a readable "
              "DOLRECOMP_LLVM_PROFILE (.profdata)\n");
      abort();
    }
    fprintf(stderr, "dolllvm: PGO use, profile %s (%s)\n",
            pgoProfilePath().c_str(), pgoProfileFingerprint().c_str());
    fflush(stderr);
    return (int)DOLLLVM_PGO_USE;
  }();
  return mode;
}

static CodeGenOptLevel codegenLevel(int level) {
  if (level <= 0)
    return CodeGenOptLevel::None;
  if (level == 1)
    return CodeGenOptLevel::Less;
  if (level == 2)
    return CodeGenOptLevel::Default;
  return CodeGenOptLevel::Aggressive;
}

static bool needsCZeroExtend(llvm::Type *type) {
  return type->isIntegerTy(1) || type->isIntegerTy(8);
}

// Clang's arm64 C ABI marks _Bool and unsigned char returns/parameters
// zeroext.  The LLVM emitter creates declarations directly, so it must attach
// the same contract explicitly rather than relying on frontend lowering that
// never ran.  Apply it to indirect hook calls as well as named helpers.
static void applyCABIAttributes(llvm::Module &module) {
  for (llvm::Function &function : module) {
    llvm::FunctionType *type = function.getFunctionType();
    // Intrinsic signatures are LLVM IR contracts, not C ABI declarations.
    // In particular, llvm.ctlz's i1 parameter is `immarg`; adding zeroext to
    // it makes the module invalid once a title contains cntlzw.
    if (!function.isIntrinsic()) {
      if (needsCZeroExtend(type->getReturnType()))
        function.addRetAttr(llvm::Attribute::ZExt);
      for (unsigned index = 0; index < type->getNumParams(); ++index)
        if (needsCZeroExtend(type->getParamType(index)))
          function.addParamAttr(index, llvm::Attribute::ZExt);
    }

    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call)
          continue;
        if (llvm::Function *callee = call->getCalledFunction();
            callee && callee->isIntrinsic())
          continue;
        llvm::FunctionType *callType = call->getFunctionType();
        if (needsCZeroExtend(callType->getReturnType()))
          call->addRetAttr(llvm::Attribute::ZExt);
        for (unsigned index = 0; index < callType->getNumParams(); ++index)
          if (needsCZeroExtend(callType->getParamType(index)))
            call->addParamAttr(index, llvm::Attribute::ZExt);
      }
    }
  }
}

static TargetMachine *targetMachine(const Target *target,
                                    const std::string &tripleName, int opt) {
  static thread_local std::string cachedTriple;
  static thread_local std::string cachedCPU;
  static thread_local std::string cachedFeatures;
  static thread_local int cachedOpt = -1;
  static thread_local std::unique_ptr<TargetMachine> cachedMachine;
  const llvm::Triple triple(tripleName);
  const std::string cpu = targetCPU(triple);
  const std::string features = targetFeatures(triple);
  if (!cachedMachine || cachedTriple != tripleName || cachedCPU != cpu ||
      cachedFeatures != features || cachedOpt != opt) {
    TargetOptions options;
    cachedMachine.reset(target->createTargetMachine(
        tripleName, cpu, features, options, Reloc::PIC_, CodeModel::Small,
        codegenLevel(opt)));
    cachedTriple = tripleName;
    cachedCPU = cpu;
    cachedFeatures = features;
    cachedOpt = opt;
  }
  return cachedMachine.get();
}

static uint32_t readLE32(const unsigned char *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

static uint32_t packedVersion(const llvm::VersionTuple &version) {
  const unsigned major = version.getMajor();
  const unsigned minor = version.getMinor().value_or(0);
  const unsigned patch = version.getSubminor().value_or(0);
  if (major > 0xffffu || minor > 0xffu || patch > 0xffu)
    return 0;
  return major << 16 | minor << 8 | patch;
}

static llvm::VersionTuple targetMinimumVersion(const llvm::Triple &triple) {
  if (triple.isiOS())
    return triple.getiOSVersion();
  if (triple.isOSDarwin()) {
    llvm::VersionTuple version;
    if (triple.getMacOSXVersion(version))
      return version;
  }
  return triple.getMinimumSupportedOSVersion();
}

static bool readFile(const char *path, std::vector<unsigned char> *bytes) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  const long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }
  bytes->resize(static_cast<size_t>(length));
  const bool ok = bytes->empty() ||
                  fread(bytes->data(), 1, bytes->size(), file) == bytes->size();
  fclose(file);
  return ok;
}

static bool matchesMachO(const std::vector<unsigned char> &bytes,
                         const llvm::Triple &triple) {
  // mach_header_64 is eight little-endian u32s.
  if (bytes.size() < 32 || readLE32(bytes.data()) != 0xfeedfacfu)
    return false;
  const uint32_t expectedCPU = triple.getArch() == llvm::Triple::aarch64
                                   ? 0x0100000cu
                                   : 0x01000007u;
  if (readLE32(bytes.data() + 4) != expectedCPU ||
      readLE32(bytes.data() + 12) != 1u) // MH_OBJECT
    return false;
  const uint32_t commands = readLE32(bytes.data() + 16);
  const uint32_t commandBytes = readLE32(bytes.data() + 20);
  if (commands > 65536u || commandBytes > bytes.size() - 32u)
    return false;

  uint32_t expectedPlatform = 0;
  if (triple.isiOS())
    expectedPlatform = triple.isSimulatorEnvironment() ? 7u : 2u;
  else if (triple.isOSDarwin())
    expectedPlatform = 1u;
  if (!expectedPlatform)
    return false;
  const uint32_t expectedMinOS = packedVersion(targetMinimumVersion(triple));
  if (!expectedMinOS)
    return false;

  size_t offset = 32;
  bool foundBuildVersion = false;
  for (uint32_t index = 0; index < commands; ++index) {
    if (offset > bytes.size() - 8)
      return false;
    const uint32_t command = readLE32(bytes.data() + offset);
    const uint32_t size = readLE32(bytes.data() + offset + 4);
    if (size < 8 || size > bytes.size() - offset)
      return false;
    if (command == 0x32u) { // LC_BUILD_VERSION
      if (size < 24 || foundBuildVersion)
        return false;
      foundBuildVersion = true;
      if (readLE32(bytes.data() + offset + 8) != expectedPlatform ||
          readLE32(bytes.data() + offset + 12) != expectedMinOS)
        return false;
    }
    offset += size;
  }
  return foundBuildVersion;
}

} // namespace

namespace dolllvm {

namespace {
// Host layout until an emission says otherwise, which is correct for every
// target that shares the host's pointer size.
DolNativeTargetLayout g_layout = dolnative_target_layout(sizeof(void *));
}  // namespace

void setTargetLayout(unsigned pointer_size) {
  g_layout = dolnative_target_layout(pointer_size);
}

const DolNativeTargetLayout &targetLayout() { return g_layout; }

size_t targetStateOffset(DolIRStateSlot slot) {
  // The only slot past the pointers. Everything else is in the prefix, where
  // every target agrees.
  if (slot == DOLIR_STATE_DOWNCOUNT)
    return g_layout.downcount;
  return dolnative_state_offset(slot);
}

}  // namespace dolllvm


extern "C" bool dolllvm_emit_object(const DolIRModule *source,
                                    const char *object_path,
                                    const DolLLVMOptions *options,
                                    FILE *diagnostics) {
  if (!source || !object_path || !diagnostics)
    return false;
  static bool initialized = [] {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();
    return true;
  }();
  (void)initialized;

  int opt = options ? options->optimization_level : 2;
  std::string tripleName =
      resolveTriple(options ? options->target_triple : nullptr);
  const llvm::Triple triple(tripleName);
  if (!supportedTarget(triple)) {
    fprintf(diagnostics,
            "dolllvm: supported targets are x86-64 Linux/Windows, arm64 "
            "macOS, arm64 iOS device/simulator triples with an explicit minimum "
            "OS, and wasm32\n");
    return false;
  }

  std::string error;
  const llvm::Target *target =
      llvm::TargetRegistry::lookupTarget(tripleName, error);
  if (!target) {
    fprintf(diagnostics, "dolllvm: %s\n", error.c_str());
    return false;
  }
  llvm::TargetMachine *machine = targetMachine(target, tripleName, opt);
  if (!machine) {
    fprintf(diagnostics, "dolllvm: failed to create target machine\n");
    return false;
  }

  // The offsets baked into the objects have to be the target's, and wasm32's
  // pointer-bearing CPUState tail does not line up with a 64-bit host's. Ask
  // the target's own DataLayout rather than inferring from the triple.
  if (!dolnative_target_layout_matches_host()) {
    fprintf(diagnostics,
            "dolllvm: the CPUState tail walker disagrees with this compiler, so "
            "every offset it produces would be wrong -- native_state_layout.h "
            "has drifted from cpu.h\n");
    return false;
  }
  const unsigned pointer_size = machine->createDataLayout().getPointerSize();
  dolllvm::setTargetLayout(pointer_size);

  llvm::LLVMContext context;
  llvm::Module module("dolrecomp_native", context);
  module.setTargetTriple(tripleName);
  module.setDataLayout(machine->createDataLayout());
  std::string diagnosticText;
  llvm::raw_string_ostream diagnosticStream(diagnosticText);
  for (u32 i = 0; i < source->function_count; i++) {
    dolllvm::FunctionEmitter emitter(
        context, module, source->functions[i],
        options ? options->function_ranges : nullptr,
        options ? options->function_range_count : 0,
        options && options->gamecube,
        options && options->symbol_prefix ? options->symbol_prefix : "");
    if (!emitter.emit(diagnosticStream)) {
      diagnosticStream.flush();
      fprintf(diagnostics, "%s", diagnosticText.c_str());
      return false;
    }
  }
  applyCABIAttributes(module);
  if (llvm::verifyModule(module, &diagnosticStream)) {
    diagnosticStream.flush();
    fprintf(diagnostics, "%s", diagnosticText.c_str());
    return false;
  }

  if (opt > 0) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    // DOLRECOMP_LLVM_TRACE_PASSES names the last pass and IR unit to start.
    // Without it an optimizer that fails to converge is indistinguishable from
    // one that is merely slow: the historical instcombine hang on this title
    // spun for 49 minutes at 1.00 core with nothing identifying the function.
    // Each line is flushed, so the last line printed is where it stopped.
    llvm::PassInstrumentationCallbacks callbacks;
    const int pgo = dolllvm_pgo_mode();
    const bool tracePasses = getenv("DOLRECOMP_LLVM_TRACE_PASSES") != nullptr;
    // P002. Counted after the pipeline has run. PGO entry counts survive the
    // later optimization passes, while checking the final defined functions is
    // less fragile than depending on pass-instrumentation callback details.
    unsigned long long matchedHere = 0;
    unsigned long long unmatchedHere = 0;
    const bool gatePgo =
        pgo == DOLLLVM_PGO_USE &&
        dolllvm_pgo_stale_policy() != DOLLLVM_PGO_STALE_OFF;
    if (tracePasses) {
      callbacks.registerBeforeNonSkippedPassCallback(
          [](llvm::StringRef pass, llvm::Any ir) {
            // Any holds a pointer to the IR unit, so any_cast<const T *> on
            // the Any* yields const T *const *.
            std::string unit = "<unknown>";
            const llvm::Function *const *fn =
                llvm::any_cast<const llvm::Function *>(&ir);
            const llvm::Module *const *mod =
                llvm::any_cast<const llvm::Module *>(&ir);
            if (fn && *fn)
              unit = (*fn)->getName().str();
            else if (mod && *mod)
              unit = (*mod)->getName().str();
            fprintf(stderr, "dolllvm: pass %s on %s\n", pass.str().c_str(),
                    unit.c_str());
            fflush(stderr);
          });
    }
    llvm::PassBuilder passBuilder(machine, llvm::PipelineTuningOptions(),
                                  std::nullopt,
                                  tracePasses ? &callbacks : nullptr);
    passBuilder.registerModuleAnalyses(mam);
    passBuilder.registerCGSCCAnalyses(cgam);
    passBuilder.registerFunctionAnalyses(fam);
    passBuilder.registerLoopAnalyses(lam);
    passBuilder.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager passes;
    // P001. Front of the pipeline, before anything has touched the emitter's
    // output. Gen and Use must observe byte-identical IR or the CFG hashes they
    // key on disagree and every function silently goes unprofiled; running both
    // here makes that identity structural rather than a property of the passes
    // in between, which are free to change without invalidating a profile.
    if (pgo == DOLLLVM_PGO_GEN) {
#if LLVM_VERSION_MAJOR >= 20
      passes.addPass(llvm::PGOInstrumentationGen(
          llvm::PGOInstrumentationType::FDO));
#else
      passes.addPass(llvm::PGOInstrumentationGen(/*IsCS=*/false));
#endif
    } else if (pgo == DOLLLVM_PGO_USE) {
      passes.addPass(llvm::PGOInstrumentationUse(pgoProfilePath()));
    }
    if (llvm::Error error =
            passBuilder.parsePassPipeline(passes, passPipeline())) {
      fprintf(diagnostics,
              "dolllvm: cannot construct optimization pipeline: %s\n",
              llvm::toString(std::move(error)).c_str());
      return false;
    }
    // The counter intrinsics have to survive the optimizer as intrinsics: once
    // lowered they are a load, an add and a store on a global, and GVN or DSE
    // will happily fold two iterations of a loop into one increment. Lower them
    // last, which is where clang lowers them and for the same reason.
    if (pgo == DOLLLVM_PGO_GEN)
      passes.addPass(llvm::InstrProfilingLoweringPass(llvm::InstrProfOptions(),
                                                      /*IsCS=*/false));
    passes.run(module, mam);

    if (gatePgo) {
      for (const llvm::Function &function : module) {
        if (function.isDeclaration())
          continue;
        if (function.getEntryCount())
          matchedHere++;
        else
          unmatchedHere++;
      }
    }

    // P002. The verdict. Under `error` a stale profile stops the build here,
    // which is the point: the failure this gate exists for is a build that
    // SUCCEEDS while training on records that no longer describe it, and every
    // downstream number then belongs to a module nobody meant to measure.
    if (gatePgo) {
      if (unmatchedHere != 0) {
        fprintf(diagnostics,
                "dolllvm: PGO profile stale for %s: %llu of %llu functions "
                "have no profile record\n",
                object_path ? object_path : "<chunk>", unmatchedHere,
                matchedHere + unmatchedHere);
        fflush(diagnostics);
        if (dolllvm_pgo_stale_policy() == DOLLLVM_PGO_STALE_ERROR) {
          fprintf(diagnostics,
                  "dolllvm: refusing to emit against a stale profile. Re-collect "
                  "it, or set DOLRECOMP_LLVM_PGO_STALE=warn to build anyway.\n");
          fflush(diagnostics);
          return false;
        }
      }
    }
  }
  if (llvm::verifyModule(module, &diagnosticStream)) {
    diagnosticStream.flush();
    fprintf(diagnostics, "%s", diagnosticText.c_str());
    return false;
  }

  if (options && options->emit_ir && options->ir_path) {
    std::error_code irError;
    llvm::raw_fd_ostream irFile(options->ir_path, irError,
                                llvm::sys::fs::OF_Text);
    if (irError) {
      fprintf(diagnostics, "dolllvm: cannot write IR: %s\n",
              irError.message().c_str());
      return false;
    }
    module.print(irFile, nullptr);
  }

  std::error_code objectError;
  llvm::raw_fd_ostream objectFile(object_path, objectError,
                                  llvm::sys::fs::OF_None);
  if (objectError) {
    fprintf(diagnostics, "dolllvm: cannot write object: %s\n",
            objectError.message().c_str());
    return false;
  }
  llvm::legacy::PassManager codegen;
  if (machine->addPassesToEmitFile(codegen, objectFile, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
    fprintf(diagnostics, "dolllvm: target cannot emit objects\n");
    return false;
  }
  codegen.run(module);
  objectFile.flush();
  return true;
}

extern "C" unsigned dolllvm_target_pointer_size(const char *requested) {
  const llvm::Triple triple(resolveTriple(requested));
  return triple.isArch32Bit() ? 4u : 8u;
}

extern "C" bool dolllvm_effective_triple(const char *requested, char *out,
                                         size_t size) {
  if (!out || size == 0)
    return false;
  const std::string triple = resolveTriple(requested);
  if (triple.size() + 1 > size)
    return false;
  memcpy(out, triple.c_str(), triple.size() + 1);
  return true;
}

extern "C" bool dolllvm_object_matches_triple(const char *path,
                                              const char *requested) {
  std::vector<unsigned char> bytes;
  if (!readFile(path, &bytes) || bytes.size() < 4)
    return false;

  const llvm::Triple triple(resolveTriple(requested));
  if (triple.isOSBinFormatCOFF())
    // IMAGE_FILE_MACHINE_AMD64, little-endian, at offset 0 of a COFF object.
    return bytes[0] == 0x64 && bytes[1] == 0x86;
  if (triple.isOSBinFormatMachO())
    return matchesMachO(bytes, triple);
  return bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' &&
         bytes[3] == 'F';
}

extern "C" const char *dolllvm_version(void) { return LLVM_VERSION_STRING; }

static bool copyMetadataString(const std::string &value, char *out,
                               size_t size) {
  if (!out || size == 0 || value.size() + 1 > size)
    return false;
  memcpy(out, value.c_str(), value.size() + 1);
  return true;
}

extern "C" bool dolllvm_target_cpu(const char *requested, char *out,
                                    size_t size) {
  return copyMetadataString(targetCPU(llvm::Triple(resolveTriple(requested))),
                            out, size);
}

extern "C" bool dolllvm_target_features(const char *requested, char *out,
                                         size_t size) {
  return copyMetadataString(
      targetFeatures(llvm::Triple(resolveTriple(requested))), out, size);
}

// Default `error`: a stale profile is a wrong measurement, not a slow one, and
// a profile that looks applied and is not costs a whole build cycle.
extern "C" int dolllvm_pgo_stale_policy(void) {
  static const int policy = [] {
    const char *requested = getenv("DOLRECOMP_LLVM_PGO_STALE");
    if (!requested || !requested[0] || !strcmp(requested, "error"))
      return (int)DOLLLVM_PGO_STALE_ERROR;
    if (!strcmp(requested, "warn"))
      return (int)DOLLLVM_PGO_STALE_WARN;
    if (!strcmp(requested, "off") || !strcmp(requested, "0"))
      return (int)DOLLLVM_PGO_STALE_OFF;
    fprintf(stderr,
            "dolllvm: DOLRECOMP_LLVM_PGO_STALE must be error, warn or off\n");
    abort();
    return (int)DOLLLVM_PGO_STALE_ERROR;
  }();
  return policy;
}

extern "C" bool dolllvm_codegen_fingerprint(const char *requested, char *out,
                                             size_t size) {
  if (!out || size == 0)
    return false;
  // Every codegen-affecting input the object cache key would otherwise miss.
  // The triple is hashed separately by the caller, which already had it.
  // Keyed to the PGO mode, and in use mode to the profile's CONTENT -- a
  // profile rewritten in place by a collection script makes its path a
  // non-identity. Absent entirely when PGO is off, which is what keeps the
  // default objects byte-identical and the existing cache valid.
  const llvm::Triple triple(resolveTriple(requested));
  const std::string fingerprint =
      std::string(LLVM_VERSION_STRING) + "|" + triple.str() + "|" +
      targetCPU(triple) + "|" + targetFeatures(triple) + "|pic|small|minos=" +
      targetMinimumVersion(triple).getAsString() + "|layout=" +
      std::to_string(dolnative_state_layout_hash()) + "|" + passPipeline() +
      (dolllvm_pgo_mode() == DOLLLVM_PGO_GEN ? "|pgo=gen" : "") +
      (dolllvm_pgo_mode() == DOLLLVM_PGO_USE
           ? "|pgo=use:" + pgoProfileFingerprint()
           : "");
  if (fingerprint.size() + 1 > size)
    return false;
  memcpy(out, fingerprint.c_str(), fingerprint.size() + 1);
  return true;
}
