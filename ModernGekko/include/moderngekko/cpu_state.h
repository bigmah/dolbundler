#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODERNGEKKO_CPU_ABI_VERSION 3u
#define GXRUNTIME_CPU_ABI_VERSION MODERNGEKKO_CPU_ABI_VERSION

typedef struct CPUState CPUState;

typedef uint64_t (*PPCExternalRead)(CPUState* cpu, uint32_t address, uint8_t size);
typedef void (*PPCExternalWrite)(CPUState* cpu, uint32_t address, uint64_t value, uint8_t size);
typedef uint32_t (*PPCExternalRead32)(CPUState* cpu, uint32_t address, uint8_t region);
typedef void (*PPCExternalWrite32)(CPUState* cpu, uint32_t address, uint32_t value,
                                   uint8_t region);
typedef void* (*PPCExternalPointer)(CPUState* cpu, uint32_t address, uint32_t size);
typedef void (*PPCInstructionFallback)(CPUState* cpu, uint32_t instruction, uint32_t address);
typedef bool (*PPCHostCall)(CPUState* cpu, uint32_t address);
typedef uint32_t (*PPCSPRRead)(CPUState* cpu, uint16_t spr, uint32_t address);
typedef void (*PPCSPRWrite)(CPUState* cpu, uint16_t spr, uint32_t value, uint32_t address);
typedef void (*PPCCacheControl)(CPUState* cpu, uint8_t operation, uint32_t address,
                                uint32_t instruction_address);

struct CPUState
{
    uint32_t gpr[32];
    double fpr[32];
    double ps1[32];
    uint32_t pc;
    uint32_t lr;
    uint32_t ctr;
    uint32_t cr;
    uint32_t xer;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t dar;
    uint32_t dsisr;
    uint32_t ear;
    uint32_t hid2;
    uint64_t timebase;
    uint32_t sr[16];
    uint32_t gqr[8];
    uint32_t exception;
    uint32_t program_exception;
    uint32_t tlb_last_vps;
    uint32_t tlb_last_index;
    uint32_t tlb_invalidate_count;
    uint32_t external_addr;
    uint32_t external_value;
    uint8_t external_rid;
    uint8_t external_read_count;
    uint8_t external_write_count;
    uint32_t reserve_addr;
    bool reserve_valid;
    uint32_t locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;
    uint8_t* ram;
    uint32_t ram_size;
    PPCExternalPointer external_pointer;
    int64_t downcount;
    uint8_t* exram;
    uint32_t exram_size;
    PPCSPRRead spr_read;
    PPCSPRWrite spr_write;
    PPCCacheControl cache_control;
    /* The Gekko's locked cache at 0xE0000000. Mirrors GXRuntime's field of the
     * same name -- this header exists so the C++ side can size CPUState without
     * including GXRuntime's, and the chassis rejects a module whose descriptor
     * disagrees about that size, so the two have to be edited together. */
    uint8_t* l1cache;
    uint32_t l1cache_size;

    /* The write-gather pipe fast path; mirrors GXRuntime's fields, see there. */
    uint8_t** gather_pipe_slot;
    uint8_t* gather_pipe_limit;
    void (*gather_pipe_flush)(CPUState* cpu);
};

#ifdef __cplusplus
}
#endif

#endif
