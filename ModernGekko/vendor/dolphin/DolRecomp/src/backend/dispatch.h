#ifndef DOLRECOMP_BACKEND_DISPATCH_H
#define DOLRECOMP_BACKEND_DISPATCH_H

#include "common/types.h"
#include <stdio.h>

typedef struct {
    u32 start;
    u32 end;
} FunctionRange;

typedef struct {
    FunctionRange* ranges;
    u32 count;
    u32 capacity;
} FunctionList;

void emit_chunk_prototype(FILE* out, u32 func_addr);
void function_list_free(FunctionList* list);
int function_list_add(FunctionList* list, u32 start, u32 end);
void emit_dispatch_helpers(FILE* out, const FunctionList* funcs, u32 entry_point);

// The chassis-ordered chunk table, for the generated code's own use. An
// indirect call or a chunk-end fallthrough resolves its target in place and
// asks the gate about *that* chunk, so the index this hands back has to be the
// chassis's: sorted by start address, which is how gen_module_tables.py numbers
// chunk_ranges. The lookup goes in the header, the table's one definition in
// the manifest translation unit; both are computed from the complete list.
void emit_chunk_index_lookup(FILE* out, const FunctionList* funcs);
void emit_chunk_table_definition(FILE* out, const FunctionList* funcs);
// The same sorted order, for the emitter: fills starts[]/ends[] (each funcs->count
// long) so a per-section emission can still name every chunk in the module.
void function_list_sorted_bounds(const FunctionList* funcs, u32* starts, u32* ends);

#endif
