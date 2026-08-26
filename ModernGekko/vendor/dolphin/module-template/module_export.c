// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"
#include "core/dispatch_gate.h"
#include "core/native_state_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DOLRECOMP_NATIVE_BACKEND_NAME
#define DOLRECOMP_NATIVE_BACKEND_NAME "c"
#define DOLRECOMP_NATIVE_TARGET_TRIPLE "host-c-compiler"
#define DOLRECOMP_NATIVE_LLVM_VERSION "not-llvm"
#define DOLRECOMP_NATIVE_CODEGEN_FINGERPRINT "legacy-c-output"
#define DOLRECOMP_NATIVE_BUILD_ID "legacy-c-output"
#endif

#ifndef MODULE_SYMBOL_PREFIX
#define MODULE_SYMBOL_PREFIX
#endif
#define MODULE_SYMBOL_INNER(prefix, name) prefix##name
#define MODULE_SYMBOL_EXPAND(prefix, name) MODULE_SYMBOL_INNER(prefix, name)
#define MODULE_SYMBOL(name) MODULE_SYMBOL_EXPAND(MODULE_SYMBOL_PREFIX, name)

#include "module_tables.inc"

StaticRecompDispatchGate MODULE_SYMBOL(dolrecomp_native_gate);

// Keep the full cross-chunk check out of line. Replicating these loads and
// branches at every generated edge substantially inflates the title's hot
// code; loop guards still read the live gate inline where their smaller check
// amortizes across iterations.
bool MODULE_SYMBOL(dolrecomp_native_gate_allows)(CPUState* ctx, u32 chunk_index)
{
    // materialize() has already moved this function's outstanding charge into
    // ctx->downcount before the gate is called. Hooks flush that accumulator
    // into the chassis's live budget and reset it to zero.
    const StaticRecompDispatchGate* gate = &MODULE_SYMBOL(dolrecomp_native_gate);
    if (!gate || !gate->chunk_open || chunk_index >= gate->chunk_count)
        return false;
    if (!gate->chunk_open[chunk_index])
        return false;
    if (!staticrecomp_dispatch_budget_allows(gate, ctx->downcount))
        return false;
    if (staticrecomp_dispatch_has_pending(gate, ctx->msr))
        return false;
    return true;
}

static void chassis_publish_gate(const StaticRecompDispatchGate* gate)
{
    if (gate && gate->chunk_count == MODULE_CHUNK_RANGE_COUNT)
        MODULE_SYMBOL(dolrecomp_native_gate) = *gate;
    else
        memset(&MODULE_SYMBOL(dolrecomp_native_gate), 0,
               sizeof(MODULE_SYMBOL(dolrecomp_native_gate)));
}

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    return dolrecomp_call(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

static StaticRecompNativeMetadata s_metadata = {
    DOLRECOMP_NATIVE_BACKEND_NAME,
    DOLRECOMP_NATIVE_TARGET_TRIPLE,
    DOLRECOMP_NATIVE_LLVM_VERSION,
    DOLRECOMP_NATIVE_CODEGEN_FINGERPRINT,
    DOLRECOMP_NATIVE_BUILD_ID,
    0,
};

static StaticRecompModuleDesc s_desc = {
    .abi_version = STATICRECOMP_ABI_VERSION,
    .cpu_abi_version = GXRUNTIME_CPU_ABI_VERSION,
    .cpu_state_size = (u32)sizeof(CPUState),
    .game_id = MODULE_GAME_ID,
    .entry_point = DOLRECOMP_ENTRY_POINT,
    .dispatch = chassis_dispatch,
    .on_state_loaded = chassis_on_state_loaded,
    .code_ranges = s_code_ranges,
    .num_code_ranges = MODULE_CODE_RANGE_COUNT,
    .smc_ranges = s_smc_ranges,
    .num_smc_ranges = MODULE_SMC_RANGE_COUNT,
    .chunk_ranges = s_chunk_ranges,
    .num_chunk_ranges = MODULE_CHUNK_RANGE_COUNT,
    .chunk_hashes = s_chunk_hashes,
    .native_metadata = &s_metadata,
    .publish_gate = chassis_publish_gate,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc*
MODULE_SYMBOL(staticrecomp_get_module)(void)
{
    s_metadata.cpu_state_layout_hash = dolnative_state_layout_hash();
    return &s_desc;
}
