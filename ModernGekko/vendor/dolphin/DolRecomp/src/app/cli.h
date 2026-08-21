#ifndef DOLRECOMP_APP_CLI_H
#define DOLRECOMP_APP_CLI_H

#include <stddef.h>
#include "common/types.h"
#include "backend/emitter.h"

typedef enum {
    DOLRECOMP_BACKEND_C,
    DOLRECOMP_BACKEND_LLVM,
    // Lowers to DolVM bytecode instead of machine code, for hosts that may not
    // create executable pages at runtime.
    DOLRECOMP_BACKEND_VM,
} DolRecompBackend;

typedef struct {
    const char* input_path;
    const char* title_id_arg;
    // Six-character disc ID stamped into a --backend vm module, so a chassis
    // can refuse one built for a different game. GameCube DOLs carry no ID of
    // their own, so it has to come from the caller.
    const char* game_id;
    const char* output_arg;
    const char* map_path;
    DolRecompCPU cpu;
    DolRecompBackend backend;
    u32 jobs;
    u32 rel_base;
    int gamecube_mode;
    int cpu_explicit;
    int rel_base_set;
    int setup_mode;
    int show_help;
} CliOptions;

void print_usage(const char* argv0);
int is_title_id(const char* text);
int is_title_id_length_valid(const char* text);
int parse_cpu_name(const char* text, DolRecompCPU* cpu);
const char* cpu_display_name(DolRecompCPU cpu);
void copy_title_id(char* out, size_t out_size, const char* title_id);
int parse_job_count(const char* text, u32* jobs);
int parse_u32_arg(const char* text, const char* name, u32* value_out);
int parse_cli(int argc, char** argv, CliOptions* opts);

#endif
