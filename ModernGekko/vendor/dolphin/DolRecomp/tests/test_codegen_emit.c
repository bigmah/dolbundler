#include <stdio.h>
#include <stdlib.h>

#include "../src/backend/dispatch.h"
#include "../src/backend/emitter.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"
#include "dolrecomp_opcode_table.h"

#define BASE 0x80003000u


int main(int argc, char** argv) {
    const int count = DOLRECOMP_OPCODE_RAW_COUNT;
    FILE* out = stdout;

    if ((PPC_OP_COUNT - 1) != count) {
        fprintf(stderr, "opcode count mismatch: enum has %d, table has %d\n",
                PPC_OP_COUNT - 1, count);
        return 1;
    }

    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            perror(argv[1]);
            return 1;
        }
    }

    PPCInst* insts = (PPCInst*)calloc((size_t)count, sizeof(PPCInst));
    if (!insts) {
        if (out != stdout) fclose(out);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        insts[i] = ppc_decode(dolrecomp_opcode_raws[i], BASE + (u32)i * 4u);
        if (insts[i].op == PPC_OP_UNKNOWN) {
            fprintf(stderr, "raw 0x%08X decoded as unknown\n", dolrecomp_opcode_raws[i]);
            free(insts);
            if (out != stdout) fclose(out);
            return 1;
        }
    }

    emit_header(out);
    if (!emit_function(out, insts, (u32)count, BASE))
        return 1;

    PPCInst external_branch[3];
    external_branch[0] = ppc_decode(0x48001000, BASE + 0x1000);
    external_branch[1] = ppc_decode(0x41821000, BASE + 0x1004);
    external_branch[2] = ppc_decode(0x4E800020, BASE + 0x1008);
    if (!emit_function(out, external_branch, 3, BASE + 0x1000))
        return 1;

    PPCInst adjacent_branch[3];
    adjacent_branch[0] = ppc_decode(0x48001000, BASE + 0x100C);
    adjacent_branch[1] = ppc_decode(0x41821000, BASE + 0x1010);
    adjacent_branch[2] = ppc_decode(0x4E800020, BASE + 0x1014);
    if (!emit_function(out, adjacent_branch, 3, BASE + 0x100C))
        return 1;

    PPCInst linked_lr_branch = ppc_decode(0x4E800021, BASE + 0x1018);
    if (!emit_function(out, &linked_lr_branch, 1, BASE + 0x1018))
        return 1;

    PPCInst native_loop[4];
    native_loop[0] = ppc_decode(0x3863FFFFu, BASE + 0x1020);
    native_loop[1] = ppc_decode(0x2C030000u, BASE + 0x1024);
    native_loop[2] = ppc_decode(0x4082FFF8u, BASE + 0x1028);
    native_loop[3] = ppc_decode(0x4E800020u, BASE + 0x102C);
    if (!emit_function(out, native_loop, 4, BASE + 0x1020))
        return 1;

    PPCInst local_call[4];
    local_call[0] = ppc_decode(0x48000009u, BASE + 0x1030);
    local_call[1] = ppc_decode(0x48000008u, BASE + 0x1034);
    local_call[2] = ppc_decode(0x4E800020u, BASE + 0x1038);
    local_call[3] = ppc_decode(0x4E800020u, BASE + 0x103C);
    if (!emit_function(out, local_call, 4, BASE + 0x1030))
        return 1;

    PPCInst memory_loop[6];
    memory_loop[0] = ppc_decode(0x80850000u, BASE + 0x1040);
    memory_loop[1] = ppc_decode(0x38A50004u, BASE + 0x1044);
    memory_loop[2] = ppc_decode(0x3863FFFFu, BASE + 0x1048);
    memory_loop[3] = ppc_decode(0x2C030000u, BASE + 0x104C);
    memory_loop[4] = ppc_decode(0x4082FFF0u, BASE + 0x1050);
    memory_loop[5] = ppc_decode(0x4E800020u, BASE + 0x1054);
    if (!emit_function(out, memory_loop, 6, BASE + 0x1040))
        return 1;

    PPCInst paired_compare[2];
    paired_compare[0] = ppc_decode((4u << 26) | (2u << 23) | (1u << 16) |
                                   (2u << 11) | (32u << 1), BASE + 0x1060);
    paired_compare[1] = ppc_decode(0x4E800020u, BASE + 0x1064);
    if (!emit_function(out, paired_compare, 2, BASE + 0x1060))
        return 1;

    PPCInst record_float[2];
    record_float[0] = ppc_decode((59u << 26) | (3u << 21) | (1u << 16) |
                                 (2u << 11) | (21u << 1) | 1u,
                                 BASE + 0x1068);
    record_float[1] = ppc_decode(0x4E800020u, BASE + 0x106C);
    if (!emit_function(out, record_float, 2, BASE + 0x1068))
        return 1;

    PPCInst paired_merge[2];
    paired_merge[0] = ppc_decode((4u << 26) | (5u << 21) | (1u << 16) |
                                 (2u << 11) | (528u << 1), BASE + 0x1070);
    paired_merge[1] = ppc_decode(0x4E800020u, BASE + 0x1074);
    if (!emit_function(out, paired_merge, 2, BASE + 0x1070))
        return 1;

    FunctionList funcs = {0};
    if (!function_list_add(&funcs, BASE, BASE + (u32)count * 4u) ||
        !function_list_add(&funcs, BASE + 0x1000, BASE + 0x100C) ||
        !function_list_add(&funcs, BASE + 0x100C, BASE + 0x1018) ||
        !function_list_add(&funcs, BASE + 0x1018, BASE + 0x101C) ||
        !function_list_add(&funcs, BASE + 0x1020, BASE + 0x1030) ||
        !function_list_add(&funcs, BASE + 0x1030, BASE + 0x1040) ||
        !function_list_add(&funcs, BASE + 0x1040, BASE + 0x1058) ||
        !function_list_add(&funcs, BASE + 0x1060, BASE + 0x1068) ||
        !function_list_add(&funcs, BASE + 0x1068, BASE + 0x1070) ||
        !function_list_add(&funcs, BASE + 0x1070, BASE + 0x1078)) {
        function_list_free(&funcs);
        free(insts);
        if (out != stdout) fclose(out);
        return 1;
    }
    emit_dispatch_helpers(out, &funcs, BASE);
    function_list_free(&funcs);

    emit_footer(out);

    free(insts);
    if (out != stdout) fclose(out);
    return 0;
}
