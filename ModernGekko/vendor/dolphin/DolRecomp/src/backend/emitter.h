#ifndef DOLRECOMP_EMITTER_H
#define DOLRECOMP_EMITTER_H

#include "../common/types.h"
#include "../frontend/decoder.h"
#include <stdio.h>

typedef enum {
    DOLRECOMP_CPU_GEKKO,
    DOLRECOMP_CPU_BROADWAY,
    DOLRECOMP_CPU_ESPRESSO,
} DolRecompCPU;

// Split C emitter used by the command-line recompiler.

// emit the boilerplate header (includes, typedefs, etc)
void emit_header(FILE* out);
void emit_header_for_cpu(FILE* out, DolRecompCPU cpu);

// Register chunk entries for the opt-in cross-chunk direct-call experiment.
// Direct calls bypass chassis dispatch checks and must not be enabled by a
// runtime that validates mutable guest code there. Call before worker emission;
// passing count == 0 restores the safe return-to-chassis form.
void emit_set_chunk_table(const u32* starts, u32 count);

// HLE generation only: lower a non-local `bl` whose continuation is local to
// a DOLRECOMP_OUTCALL(target, return, label) macro instead of a plain leave,
// so the stand-in build can resolve cross-pattern calls at run time. Never
// used for whole-module output -- pattern addresses from different titles
// overlap, so nothing may bind at build time.
void emit_set_hle_outcalls(bool enabled);

// Give the host a chance to intercept a `bl` whose target sits inside the same
// emitted chunk. Off by default: it costs one predictable branch per intra-chunk
// call, and a runtime with no SDK intercepts (ctx->host_call == NULL) gains
// nothing. Required by any host that HLEs SDK functions by guest address --
// without it a chunked build silently runs the guest's own SDK code instead.
void emit_set_hle_local_calls(bool enabled);

// emit a single recompiled function as C code
bool emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr);

// emit a single instruction as C code
void emit_instruction(FILE* out, const PPCInst* inst);

// emit the boilerplate footer
void emit_footer(FILE* out);

#endif /* DOLRECOMP_EMITTER_H */
