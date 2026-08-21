// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emit the C backend's arm of the differential test: one generated function per
// opcode, plus a table so the harness can call them by index.

#include "../src/backend/emitter.h"
#include "../src/frontend/decoder.h"
#include "../src/ir/dolir_builder.h"
#include "dolvm_diff_layout.h"

#include <stdio.h>
#include <stdlib.h>

// An opcode is worth chaining into a group if the IR builder lowers it and it
// falls through, so that a group really is one straight-line block. Asking the
// IR rather than keeping a list means the set tracks the builder.
static bool straight_line(u32 raw, u32 address) {
    PPCInst pair[2];
    pair[0] = ppc_decode(raw, address);
    pair[1] = ppc_decode(0x4E800020u, address + 4u);
    if (dolvm_diff_ir_path_differs(pair[0].op))
        return false;
    DolIRModule ir;
    dolir_module_init(&ir);
    bool ok = dolir_build_chunk(&ir, pair, 2, address) &&
              ir.functions[0].blocks[0].terminator.kind == DOLIR_TERM_BRANCH;
    dolir_module_free(&ir);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_dolvm_diff_gen <output.c>\n");
        return 2;
    }
    const int count = DOLRECOMP_OPCODE_RAW_COUNT;
    if ((PPC_OP_COUNT - 1) != count) {
        fprintf(stderr, "opcode count mismatch: enum has %d, table has %d\n",
                PPC_OP_COUNT - 1, count);
        return 1;
    }
    FILE* out = fopen(argv[1], "w");
    if (!out) {
        perror(argv[1]);
        return 1;
    }
    emit_header(out);

    for (int i = 0; i < count; i++) {
        u32 address = dolvm_diff_address(i);
        PPCInst pair[2];
        pair[0] = ppc_decode(dolrecomp_opcode_raws[i], address);
        pair[1] = ppc_decode(0x4E800020u, address + 4u);
        if (pair[0].op == PPC_OP_UNKNOWN) {
            fprintf(stderr, "raw 0x%08X decoded as unknown\n",
                    dolrecomp_opcode_raws[i]);
            fclose(out);
            return 1;
        }
        if (!emit_function(out, pair, 2, address)) {
            fclose(out);
            return 1;
        }
    }

    fprintf(out, "void (*const dolvm_diff_native[%d])(CPUState*) = {\n", count);
    for (int i = 0; i < count; i++)
        fprintf(out, "    func_%08X,\n", dolvm_diff_address(i));
    fprintf(out, "};\n\n");

    // Groups: runs of straight-line opcodes, so the bytecode has real
    // superblocks and every address inside one is an entry the harness can
    // compare. The word list is emitted alongside so the harness lowers exactly
    // what the C backend was handed.
    u32* pending = (u32*)malloc(sizeof(u32) * DOLVM_DIFF_GROUP_SIZE);
    if (!pending) {
        fclose(out);
        return 1;
    }
    u32 pending_count = 0;
    u32 groups = 0;
    FILE* words = tmpfile();
    if (!words) {
        free(pending);
        fclose(out);
        return 1;
    }
    // Whole groups only: the harness reads a fixed-length word table, so a short
    // tail group would shift everything after it.
    for (int i = 0; i < count; i++) {
        u32 address = dolvm_diff_address(i);
        if (!straight_line(dolrecomp_opcode_raws[i], address))
            continue;
        pending[pending_count++] = dolrecomp_opcode_raws[i];
        if (pending_count < DOLVM_DIFF_GROUP_SIZE)
            continue;

        u32 base = dolvm_diff_group_address(groups);
        PPCInst body[DOLVM_DIFF_GROUP_SIZE + 1u];
        for (u32 n = 0; n < pending_count; n++) {
            body[n] = ppc_decode(pending[n], base + n * 4u);
            fprintf(words, "    0x%08Xu,\n", pending[n]);
        }
        body[pending_count] = ppc_decode(0x4E800020u, base + pending_count * 4u);
        fprintf(words, "    0x4E800020u,\n");
        if (!emit_function(out, body, pending_count + 1u, base)) {
            free(pending);
            fclose(words);
            fclose(out);
            return 1;
        }
        groups++;
        pending_count = 0;
    }
    free(pending);

    fprintf(out, "const u32 dolvm_diff_group_count = %u;\n", groups);
    fprintf(out, "const u32 dolvm_diff_group_length = %u;\n",
            DOLVM_DIFF_GROUP_SIZE + 1u);
    fprintf(out, "const u32 dolvm_diff_group_words[] = {\n");
    rewind(words);
    for (int c = fgetc(words); c != EOF; c = fgetc(words))
        fputc(c, out);
    fclose(words);
    fprintf(out, "};\n");
    fprintf(out, "void (*const dolvm_diff_group_native[])(CPUState*) = {\n");
    for (u32 g = 0; g < groups; g++)
        fprintf(out, "    func_%08X,\n", dolvm_diff_group_address(g));
    fprintf(out, "};\n");
    fprintf(out, "#endif\n");
    fclose(out);
    return 0;
}
