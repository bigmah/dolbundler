// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C++ side of the chassis cannot include a CPUState header. It already has
// one -- ModernGekko's own mirror in module_abi.h -- and GXRuntime's shares its
// include guard, so pulling both into one translation unit gets you whichever
// arrived first and none of what the other declared. Everything here is
// therefore stated in terms of an incomplete `struct CPUState`, which is
// exactly the type the module ABI's dispatch pointer is spelled with.
//
// One module per process: the ABI's dispatch is a bare function pointer with
// nowhere to hang a context, so the loaded module is file scope in the .c.

#ifndef MODERNGEKKO_DOLVM_BRIDGE_H
#define MODERNGEKKO_DOLVM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct StaticRecompDispatchGate;

typedef struct DolVMBridgeRange
{
    uint32_t start;
    uint32_t end;
} DolVMBridgeRange;

// What the chassis needs to describe the module to itself. Every pointer is
// owned by the bridge and stays valid until dolvm_bridge_close().
typedef struct DolVMBridgeInfo
{
    const char* game_id;
    uint32_t entry_point;
    uint32_t state_layout_hash;
    // Guest ranges the module covers, ascending and non-overlapping. These are
    // also the chunks: a region is what the recompiler lowered in one go, and
    // so is the granule the SMC guard can retire.
    const DolVMBridgeRange* regions;
    const uint64_t* region_hashes;
    uint32_t region_count;
    const DolVMBridgeRange* smc_ranges;
    uint32_t smc_count;
    int direct_calls;
} DolVMBridgeInfo;

// Loads a .dvm. Returns 0 and fills `error` on failure.
int dolvm_bridge_open(const char* path, DolVMBridgeInfo* info, char* error,
                      size_t error_size);
void dolvm_bridge_close(void);

// The two module ABI entry points, over whatever dolvm_bridge_open loaded.
int dolvm_bridge_dispatch(struct CPUState* ctx, uint32_t address);
void dolvm_bridge_on_state_loaded(struct CPUState* ctx);

// The chassis's dispatch gate (core/dispatch_gate.h), or NULL to withdraw it.
// With one installed the interpreter follows calls and returns into any chunk
// the gate says is open and runs to the end of the chassis's slice; without
// one it returns at every one of them, which is also what it does when the
// gate's chunk count does not match the module's regions.
void dolvm_bridge_set_gate(const struct StaticRecompDispatchGate* gate);

#ifdef __cplusplus
}
#endif

#endif
