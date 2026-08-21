// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_BACKEND_VM_DOLVM_PIPELINE_H
#define DOLRECOMP_BACKEND_VM_DOLVM_PIPELINE_H

#include "analysis/code_section.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lower every code section into one .dvm beside `output_path`.
int emit_code_sections_vm(const LoadedCodeSection* sections, u32 section_count,
                          const char* output_path, u32 entry_point,
                          const char* game_id);

#ifdef __cplusplus
}
#endif

#endif
