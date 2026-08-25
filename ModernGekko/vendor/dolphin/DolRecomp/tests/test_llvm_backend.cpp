#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed: %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (0)

static u32 encode_spr(u16 spr) {
    return ((spr & 31u) << 5) | ((spr >> 5) & 31u);
}

static u32 mfspr(u8 destination, u16 spr) {
    return 0x7C0002A6u | (u32(destination) << 21) | (encode_spr(spr) << 11);
}

static u32 mtspr(u8 source, u16 spr) {
    return 0x7C0003A6u | (u32(source) << 21) | (encode_spr(spr) << 11);
}

static bool add_chunk(DolIRModule* module, const u32* words, u32 count,
                      u32 address) {
    PPCInst* instructions = new PPCInst[count];
    for (u32 i = 0; i < count; i++)
        instructions[i] = ppc_decode(words[i], address + i * 4u);
    const bool result = dolir_build_chunk(module, instructions, count, address);
    delete[] instructions;
    return result;
}

static void append_le32(std::vector<unsigned char>* bytes, u32 value) {
    for (u32 byte = 0; byte < 4; ++byte)
        bytes->push_back(static_cast<unsigned char>(value >> (byte * 8)));
}

static bool write_macho_fixture(const std::string& path, u32 cpu, u32 platform,
                                u32 minos) {
    std::vector<unsigned char> bytes;
    append_le32(&bytes, 0xfeedfacfu);
    append_le32(&bytes, cpu);
    append_le32(&bytes, 0u);
    append_le32(&bytes, 1u);      // MH_OBJECT
    append_le32(&bytes, 1u);
    append_le32(&bytes, 24u);
    append_le32(&bytes, 0u);
    append_le32(&bytes, 0u);
    append_le32(&bytes, 0x32u);   // LC_BUILD_VERSION
    append_le32(&bytes, 24u);
    append_le32(&bytes, platform);
    append_le32(&bytes, minos);
    append_le32(&bytes, 0u);
    append_le32(&bytes, 0u);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return output.good();
}

int main(int argc, char** argv) {
    CHECK(argc == 3 || argc == 4);
    const char* target = argc == 4 ? argv[3] : nullptr;
    DolIRModule module;
    dolir_module_init(&module);

    const u32 main_words[] = {
        0x38600000u, 0x00000000u, 0x38800000u, 0x7C841A14u,
        0x90610000u, 0x38630001u, 0x2C03000Au, 0x4180FFF4u,
        0xEE32A4FAu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, main_words, 10, 0x80001000u));

    const u32 spr_words[] = {
        mtspr(3, 273), mfspr(4, 273), 0x4E800020u,
    };
    CHECK(add_chunk(&module, spr_words, 3, 0x80002000u));

    const u32 segment_words[] = {
        0x7DC401A4u, 0x7D6304A6u, 0x7DE081E4u, 0x7D806D26u,
        0x4E800020u,
    };
    CHECK(add_chunk(&module, segment_words, 5, 0x80002100u));

    const u32 fpscr_words[] = {
        0xFFE0004Cu, 0xFDA0048Eu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, fpscr_words, 3, 0x80002200u));

    const u32 lswx_words[] = {
        0x7D34AC2Au, 0x4E800020u,
    };
    CHECK(add_chunk(&module, lswx_words, 2, 0x80002300u));

    const u32 cache_words[] = {
        0x7C11906Cu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, cache_words, 2, 0x80002400u));
    CHECK(module.functions[module.function_count - 1].blocks[0].cycle_cost == 5u);

    const u32 icbi_words[] = {
        0x7C1BE7ACu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, icbi_words, 2, 0x80002480u));
    CHECK(module.functions[module.function_count - 1].blocks[0].cycle_cost == 4u);

    const u32 trap_words[] = {
        0x0C85FFFEu, 0x38630001u, 0x4E800020u,
    };
    CHECK(add_chunk(&module, trap_words, 3, 0x80002500u));

    const u32 sc_words[] = {0x44000002u};
    CHECK(add_chunk(&module, sc_words, 1, 0x80002600u));

    const u32 rfi_words[] = {0x4C000064u};
    CHECK(add_chunk(&module, rfi_words, 1, 0x80002700u));

    const u32 dcbz_l_words[] = {
        0x100537ECu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, dcbz_l_words, 2, 0x80002800u));

    const u32 ecowx_words[] = {
        0x7D6C6B6Cu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, ecowx_words, 2, 0x80002900u));

    const u32 float_words[] = {
        0xEC22182Au, 0xEC853028u, 0xECE80272u, 0xED4B6024u,
        0xFDAE782Au, 0xFE119028u, 0xFE740572u, 0xFED7C024u,
        0x4E800020u,
    };
    CHECK(add_chunk(&module, float_words, 9, 0x80002A00u));

    const u32 paired_words[] = {
        0x1022182Au, 0x10E80272u, 0x11AE83FAu, 0x10A03030u,
        0x110D7000u, 0x10853460u, 0x4E800020u,
    };
    CHECK(add_chunk(&module, paired_words, 7, 0x80002B00u));

    const u32 paired_madds_words[] = {
        0xEC22182Au, 0xEC853028u, 0xECE80272u,
        0x123204D8u, 0x1295059Au,
        0x12F8D65Cu, 0x137CF75Eu, 0x4E800020u,
    };
    CHECK(add_chunk(&module, paired_madds_words, 8, 0x80002B40u));

    const u32 psq_words[] = {0xE0230000u, 0x4E800020u};
    CHECK(add_chunk(&module, psq_words, 2, 0x80002B80u));

    // Runtime boundaries must not reset the dispatcher budget.
    const u32 budget_words[] = {
        0x38630001u, 0x00000000u, 0x2C032710u, 0x4180FFF4u, 0x4E800020u,
    };
    CHECK(add_chunk(&module, budget_words, 5, 0x80002C00u));

    // External tail branches share the same budget across chunks.
    const u32 cross_chunk_a[] = {0x48000100u};
    const u32 cross_chunk_b[] = {0x4BFFFF00u};
    CHECK(add_chunk(&module, cross_chunk_a, 1, 0x80002D00u));
    CHECK(add_chunk(&module, cross_chunk_b, 1, 0x80002E00u));

    // cntlzw lowers through llvm.ctlz.i32, whose i1 `is_zero_poison`
    // parameter is an LLVM immarg rather than a C _Bool argument. The C ABI
    // attribute pass must leave intrinsic declarations and calls untouched.
    const u32 ctlz_words[] = {0x7F9B0034u, 0x4E800020u};
    CHECK(add_chunk(&module, ctlz_words, 2, 0x80002F00u));

    // A dcbz loop exercises a helper call inside a guarded native back edge.
    // Budget exits must preserve the architectural loop header and CTR so
    // repeated dispatcher entries complete the exact requested range.
    const u32 canonical_loop_words[] = {
        0x7C8903A6u, 0x7C001BACu, 0x38630020u, 0x4200FFF8u, 0x4E800020u,
    };
    CHECK(add_chunk(&module, canonical_loop_words, 5, 0x80003000u));

    CHECK(dolir_verify(&module, stderr));
    DolLLVMOptions options{};
    options.target_triple = target;
    options.optimization_level = 2;
    options.verify = 1;
    options.emit_ir = 1;
    options.ir_path = argv[2];
    // Deliberately model DOL section order rather than address order. The gate
    // index is an ABI chunk-table index and must still use address rank.
    const DolLLVMFunctionRange ranges[] = {
        {0x80002E00u, 0x80002E04u},
        {0x80002D00u, 0x80002D04u},
    };
    options.function_ranges = ranges;
    options.function_range_count = 2;
    CHECK(dolllvm_emit_object(&module, argv[1], &options, stderr));
    CHECK(dolllvm_object_matches_triple(argv[1], target));

    DolLLVMOptions simulator = options;
    simulator.emit_ir = 0;
    simulator.ir_path = nullptr;
    simulator.target_triple = "arm64-apple-ios17.0-simulator";
    const std::string simulatorPath = std::string(argv[1]) + ".simulator";
    CHECK(dolllvm_emit_object(&module, simulatorPath.c_str(), &simulator,
                              stderr));
    CHECK(dolllvm_object_matches_triple(simulatorPath.c_str(),
                                        simulator.target_triple));
    CHECK(!dolllvm_object_matches_triple(simulatorPath.c_str(),
                                         "arm64-apple-ios17.0"));

    DolLLVMOptions rejected = simulator;
    const std::string rejectedPath = std::string(argv[1]) + ".rejected";
    rejected.target_triple = "arm64e-apple-ios17.0";
    CHECK(!dolllvm_emit_object(&module, rejectedPath.c_str(), &rejected,
                               stderr));
    rejected.target_triple = "arm64-apple-tvos17.0";
    CHECK(!dolllvm_emit_object(&module, rejectedPath.c_str(), &rejected,
                               stderr));

    std::ifstream ir(argv[2]);
    const std::string irText((std::istreambuf_iterator<char>(ir)),
                             std::istreambuf_iterator<char>());
    CHECK(irText.find("define hidden void @func_80001000") !=
          std::string::npos);
    CHECK(irText.find("@llvm.ctlz.i32") != std::string::npos);
    CHECK(irText.find("@llvm.fma.") != std::string::npos);

    char iosFingerprint[2048]{};
    char macFingerprint[2048]{};
    char simulatorFingerprint[2048]{};
#if defined(_WIN32)
    CHECK(_putenv_s("DOLRECOMP_LLVM_CPU", "apple-a16") == 0);
    CHECK(_putenv_s("DOLRECOMP_LLVM_FEATURES", "") == 0);
#else
    CHECK(setenv("DOLRECOMP_LLVM_CPU", "apple-a16", 1) == 0);
    CHECK(unsetenv("DOLRECOMP_LLVM_FEATURES") == 0);
#endif
    CHECK(dolllvm_codegen_fingerprint("arm64-apple-ios17.0", iosFingerprint,
                                     sizeof(iosFingerprint)));
    CHECK(dolllvm_codegen_fingerprint("arm64-apple-macos14.0", macFingerprint,
                                     sizeof(macFingerprint)));
    CHECK(dolllvm_codegen_fingerprint("arm64-apple-ios17.0-simulator",
                                     simulatorFingerprint,
                                     sizeof(simulatorFingerprint)));
    CHECK(std::string(iosFingerprint) != macFingerprint);
    CHECK(std::string(iosFingerprint) != simulatorFingerprint);
    CHECK(std::string(iosFingerprint).find(dolllvm_version()) !=
          std::string::npos);

    char cpuFingerprint[2048]{};
    char featureFingerprint[2048]{};
#if defined(_WIN32)
    CHECK(_putenv_s("DOLRECOMP_LLVM_CPU", "generic") == 0);
#else
    CHECK(setenv("DOLRECOMP_LLVM_CPU", "generic", 1) == 0);
#endif
    CHECK(dolllvm_codegen_fingerprint("arm64-apple-ios17.0", cpuFingerprint,
                                     sizeof(cpuFingerprint)));
    CHECK(std::string(iosFingerprint) != cpuFingerprint);
#if defined(_WIN32)
    CHECK(_putenv_s("DOLRECOMP_LLVM_FEATURES", "+crc") == 0);
#else
    CHECK(setenv("DOLRECOMP_LLVM_FEATURES", "+crc", 1) == 0);
#endif
    CHECK(dolllvm_codegen_fingerprint("arm64-apple-ios17.0",
                                     featureFingerprint,
                                     sizeof(featureFingerprint)));
    CHECK(std::string(cpuFingerprint) != featureFingerprint);

    if (target && std::string(target).find("ios") != std::string::npos &&
        std::string(target).find("simulator") == std::string::npos) {
        const std::string prefix = argv[1];
        const u32 arm64 = 0x0100000cu;
        const u32 x86_64 = 0x01000007u;
        CHECK(write_macho_fixture(prefix + ".x86", x86_64, 2u, 17u << 16));
        CHECK(write_macho_fixture(prefix + ".sim", arm64, 7u, 17u << 16));
        CHECK(write_macho_fixture(prefix + ".mac", arm64, 1u, 17u << 16));
        CHECK(write_macho_fixture(prefix + ".min", arm64, 2u, 18u << 16));
        CHECK(!dolllvm_object_matches_triple((prefix + ".x86").c_str(), target));
        CHECK(!dolllvm_object_matches_triple((prefix + ".sim").c_str(), target));
        CHECK(!dolllvm_object_matches_triple((prefix + ".mac").c_str(), target));
        CHECK(!dolllvm_object_matches_triple((prefix + ".min").c_str(), target));
    }
    dolir_module_free(&module);
    return 0;
}
