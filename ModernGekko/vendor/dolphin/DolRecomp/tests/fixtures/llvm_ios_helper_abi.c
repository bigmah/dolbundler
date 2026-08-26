#include "core/cpu.h"

bool dolrecomp_native_gate_allows(CPUState* cpu, u32 chunk_index);

// Force representative _Bool and u8 helper declarations into Clang IR.  The
// body is never linked; its declarations are the authoritative iPhoneOS C ABI
// fixture compared with the declarations built directly by the LLVM emitter.
bool dolllvm_ios_helper_abi(CPUState* cpu, u8 reg, bool flag, f64 value,
                            f64* output)
{
    ppc_fcmp(cpu, reg, value, value, flag);
    ppc_cache_control(cpu, reg, 0x80000000u, 0x80003100u);
    bool available = ppc_fp_available(cpu, 0x80003100u);
    bool fused = ppc_fma(cpu, value, value, value, flag, flag, flag, output);
    bool loaded = ppc_psq_load(cpu, reg, 0x80000000u, flag, reg, flag,
                               0x80003100u);
    bool gated = dolrecomp_native_gate_allows(cpu, 1u);
    return available && fused && loaded && gated;
}
