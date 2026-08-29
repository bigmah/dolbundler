cmake_minimum_required(VERSION 3.16)
project(DolRecomp C)

execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE DOLRECOMP_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT DOLRECOMP_REVISION)
    set(DOLRECOMP_REVISION "unknown")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(DOLRECOMP_ENABLE_LLVM "Build the LLVM native object backend" OFF)

# Discover LLVM before creating targets so its MSVC runtime choice is uniform.
if(DOLRECOMP_ENABLE_LLVM)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(LLVM CONFIG REQUIRED)
    if(LLVM_PACKAGE_VERSION VERSION_LESS 19 OR LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL 21)
        message(FATAL_ERROR "DolRecomp LLVM backend requires LLVM 19 or 20 (found ${LLVM_PACKAGE_VERSION})")
    endif()
    if(NOT "AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
        message(FATAL_ERROR
            "DolRecomp LLVM backend requires an LLVM build with the AArch64 target")
    endif()
    if(NOT "X86" IN_LIST LLVM_TARGETS_TO_BUILD)
        message(FATAL_ERROR
            "DolRecomp LLVM backend requires an LLVM build with the X86 target")
    endif()
    message(STATUS "DolRecomp LLVM ${LLVM_PACKAGE_VERSION}; targets: ${LLVM_TARGETS_TO_BUILD}")
endif()

# iOS forbids process spawning outright, and its SDK marks system() unavailable
# at compile time. The only callers are optional external-tool bridges (wit,
# curl), so they are compiled out rather than ported.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_compile_definitions(DOLRECOMP_NO_SUBPROCESS=1)
endif()

# Portable warnings for local development and CI.
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

if(NOT DEFINED ZLIB_ROOT)
    if(DEFINED ENV{DEVKITPRO})
        set(_devkitpro "$ENV{DEVKITPRO}")
        if(MSYS AND _devkitpro MATCHES "^[A-Za-z]:")
            execute_process(
                COMMAND cygpath -u "${_devkitpro}"
                OUTPUT_VARIABLE _devkitpro
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()
    elseif(EXISTS "/opt/devkitpro")
        set(_devkitpro "/opt/devkitpro")
    endif()

    if(DEFINED _devkitpro)
        set(_devkitpro_zlib "${_devkitpro}/msys2/ucrt64")
        if(EXISTS "${_devkitpro_zlib}/include/zlib.h")
            set(ZLIB_ROOT "${_devkitpro_zlib}")
        endif()
    endif()
endif()

find_package(ZLIB QUIET)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(DOLRECOMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(dr_cpu STATIC src/cpu/cpu.c)
target_include_directories(dr_cpu PUBLIC ${DOLRECOMP_SRC})
if(NOT WIN32)
    target_link_libraries(dr_cpu PUBLIC m)
endif()

add_library(dr_platform STATIC
    src/platform/fs.c
    src/platform/strutil.c
    src/platform/pathlist.c
    src/platform/process.c
)
target_include_directories(dr_platform PUBLIC ${DOLRECOMP_SRC})

add_library(dr_frontend STATIC
    src/frontend/decoder.c
    src/frontend/container/dol.c
    src/frontend/container/rel.c
    src/frontend/container/rpx.c
    src/frontend/container/disc_extract.c
)
target_include_directories(dr_frontend PUBLIC ${DOLRECOMP_SRC})
if(ZLIB_FOUND)
    target_compile_definitions(dr_frontend PUBLIC DOLRECOMP_HAVE_ZLIB)
    target_link_libraries(dr_frontend PUBLIC ZLIB::ZLIB)
endif()

add_library(dr_ir STATIC
    src/ir/dolir.c
    src/ir/dolir_builder.c
)
target_include_directories(dr_ir PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_ir PUBLIC dr_frontend)

# The interpreter half. Deliberately free of any compiler-side dependency: this
# is the code that ships inside an app that may not create executable pages.
add_library(dr_vm STATIC
    src/vm/dolvm_module.c
    src/vm/dolvm_interp.c
    src/vm/dolvm_hle_native.c
)
target_include_directories(dr_vm PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_vm PUBLIC dr_cpu)
if(NOT MSVC)
    # The dispatch loop's frame holds a 2 KB register file, so it gets a stack
    # guard by default -- a load, a store and a compare on a function called
    # once per thirty guest instructions. Nothing here writes an array by an
    # index the loader has not already bounds-checked.
    target_compile_options(dr_vm PRIVATE -fno-stack-protector)
endif()

# The compiler-side half: DolIR optimization and bytecode lowering.
add_library(dr_vm_backend STATIC
    src/backend/vm/dolvm_opt.c
    src/backend/vm/dolvm_emit.c
    src/backend/vm/dolvm_hle_match.c
    src/backend/vm/dolvm_pipeline.c
)
target_include_directories(dr_vm_backend PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_vm_backend PUBLIC dr_ir dr_cpu dr_analysis)

if(DOLRECOMP_ENABLE_LLVM)
    add_library(dr_llvm STATIC
        src/backend/llvm/llvm_backend.cpp
        src/backend/llvm/llvm_control_flow.cpp
        src/backend/llvm/llvm_function_emitter.cpp
        src/backend/llvm/llvm_memory_lowering.cpp
        src/backend/llvm/llvm_runtime_lowering.cpp
    )
    target_include_directories(dr_llvm PUBLIC ${DOLRECOMP_SRC})
    target_include_directories(dr_llvm PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime/include")
    target_include_directories(dr_llvm SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(dr_llvm PUBLIC DOLRECOMP_ENABLE_LLVM=1)
    target_compile_definitions(dr_llvm PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(dr_llvm PUBLIC dr_ir)
    if(MINGW)
        find_library(DOLRECOMP_LLVM_SHARED_IMPORT
            NAMES LLVM-${LLVM_VERSION_MAJOR} LLVM
            HINTS ${LLVM_LIBRARY_DIRS}
            NO_DEFAULT_PATH)
        if(DOLRECOMP_LLVM_SHARED_IMPORT MATCHES "[.]dll[.]a$")
            target_link_libraries(dr_llvm PRIVATE
                "${DOLRECOMP_LLVM_SHARED_IMPORT}")
        else()
            llvm_map_components_to_libnames(DOLRECOMP_LLVM_LIBS
                Core Support Analysis Passes Target MC native nativecodegen)
            target_link_libraries(dr_llvm PRIVATE ${DOLRECOMP_LLVM_LIBS})
        endif()
    else()
        # WebAssembly alongside the others: wasm is a target this backend has to
        # reach for the browser and iOS-web builds, and its codegen libraries
        # have to be linked in for LLVMInitializeWebAssemblyTarget to resolve.
        llvm_map_components_to_libnames(DOLRECOMP_LLVM_LIBS
            Core Support Analysis Passes Target MC
            AArch64CodeGen AArch64Desc AArch64Info AArch64Utils
            X86CodeGen X86Desc X86Info
            WebAssemblyCodeGen WebAssemblyDesc WebAssemblyInfo
            WebAssemblyAsmParser)
        target_link_libraries(dr_llvm PRIVATE ${DOLRECOMP_LLVM_LIBS})
    endif()
endif()

add_library(dr_analysis STATIC
    src/analysis/embedded_data.c
    src/analysis/smc.c
    src/analysis/symbol_map.c
)
target_include_directories(dr_analysis PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_analysis PUBLIC dr_frontend)

add_library(dr_backend STATIC
    src/backend/c_cfg.c
    src/backend/emitter.c
    src/backend/dispatch.c
    src/backend/codegen.c
    src/backend/symbols.c
)
target_include_directories(dr_backend PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_backend PUBLIC dr_frontend dr_analysis)
if(NOT WIN32)
    target_link_libraries(dr_backend PUBLIC Threads::Threads)
endif()

add_library(dr_app STATIC
    src/app/cli.c
    src/app/database.c
    src/app/setup.c
    src/app/paths.c
    src/app/pipeline.c
)
target_include_directories(dr_app PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_app PUBLIC dr_backend dr_analysis dr_frontend dr_platform dr_ir dr_vm_backend)
if(DOLRECOMP_ENABLE_LLVM)
    target_link_libraries(dr_app PUBLIC dr_llvm)
    target_include_directories(dr_app PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime/include")
    target_compile_definitions(dr_app PRIVATE
        DOLRECOMP_REVISION="${DOLRECOMP_REVISION}")
endif()

add_executable(dolrecomp src/app/main.c)
target_link_libraries(dolrecomp PRIVATE dr_app)
if(DOLRECOMP_ENABLE_LLVM)
    set_property(TARGET dolrecomp PROPERTY LINKER_LANGUAGE CXX)
    # Large LLVM modules exceed MSVC's default 1 MiB stack.
    if(MSVC)
        target_link_options(dolrecomp PRIVATE /STACK:8388608)
    endif()
endif()

add_executable(dolir_stats tools/dolir_stats.c)
target_link_libraries(dolir_stats PRIVATE dr_ir dr_frontend dr_analysis)

add_executable(dolvm_dis tools/dolvm_dis.c)
target_link_libraries(dolvm_dis PRIVATE dr_vm)

# The interpreter measured against the C backend on identical guest programs.
# The native arm is generated at build time by the C emitter and compiled by the
# host compiler, so both arms really are the same instructions.
add_executable(dolvm_hle_gen tools/dolvm_hle_gen.c)
target_include_directories(dolvm_hle_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dolvm_hle_gen PRIVATE dr_backend dr_frontend)

add_executable(dolvm_bench_gen tools/dolvm_bench_gen.c)
target_include_directories(dolvm_bench_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dolvm_bench_gen PRIVATE dr_backend dr_frontend)

set(DOLVM_BENCH_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/dolvm_bench_generated.c)
add_custom_command(
    OUTPUT ${DOLVM_BENCH_SOURCE}
    COMMAND dolvm_bench_gen ${DOLVM_BENCH_SOURCE}
    DEPENDS dolvm_bench_gen
    VERBATIM
)
add_executable(dolvm_bench tools/dolvm_bench.c ${DOLVM_BENCH_SOURCE})
target_include_directories(dolvm_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dolvm_bench PRIVATE dr_vm dr_vm_backend dr_ir dr_cpu)

enable_testing()

add_executable(test_opcodes tests/test_opcodes.c)
target_link_libraries(test_opcodes PRIVATE dr_frontend)
add_test(NAME opcodes COMMAND test_opcodes)

add_executable(test_cross_check tests/test_cross_check.c)
target_link_libraries(test_cross_check PRIVATE dr_frontend)
add_test(NAME cross_check COMMAND test_cross_check)

add_executable(test_dol tests/test_dol.c)
target_link_libraries(test_dol PRIVATE dr_frontend)
add_test(NAME dol COMMAND test_dol)

add_executable(test_rel tests/test_rel.c)
target_link_libraries(test_rel PRIVATE dr_frontend)
add_test(NAME rel COMMAND test_rel)

add_executable(test_rpx tests/test_rpx.c)
target_link_libraries(test_rpx PRIVATE dr_frontend)
add_test(NAME rpx COMMAND test_rpx)

add_executable(test_disc_extract tests/test_disc_extract.c)
target_include_directories(test_disc_extract PRIVATE ${DOLRECOMP_SRC})
add_test(NAME disc_extract
    COMMAND test_disc_extract
        $<SHELL_PATH:$<TARGET_FILE:dolrecomp>>
        $<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/disc_extract_test>
)

add_executable(test_pc_reference tests/test_pc_reference.c)
target_link_libraries(test_pc_reference PRIVATE dr_frontend dr_cpu)
add_test(NAME pc_reference COMMAND test_pc_reference)

add_executable(test_jumptables tests/test_jumptables.c)
target_link_libraries(test_jumptables PRIVATE dr_backend dr_cpu)
add_test(NAME jumptables COMMAND test_jumptables)

add_executable(test_dispatch tests/test_dispatch.c)
target_link_libraries(test_dispatch PRIVATE dr_backend)
add_test(NAME dispatch COMMAND test_dispatch)

add_executable(test_c_cfg tests/test_c_cfg.c)
target_link_libraries(test_c_cfg PRIVATE dr_backend)
add_test(NAME c_cfg COMMAND test_c_cfg)

add_executable(test_symbols tests/test_symbols.c)
target_link_libraries(test_symbols PRIVATE dr_backend)
add_test(NAME symbols COMMAND test_symbols
    $<SHELL_PATH:${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codewarrior.map>)

add_executable(test_codegen_emit tests/test_codegen_emit.c)
target_link_libraries(test_codegen_emit PRIVATE dr_backend)
add_test(NAME codegen_compile
    COMMAND ${CMAKE_COMMAND}
        -DGENERATOR_EXE=$<TARGET_FILE:test_codegen_emit>
        -DOUTPUT_C=${CMAKE_CURRENT_BINARY_DIR}/codegen_check.c
        -DREPO_SRC=${CMAKE_CURRENT_SOURCE_DIR}/src
        -DHOST_GENERATOR=${CMAKE_GENERATOR}
        -DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -DHOST_C_COMPILER=${CMAKE_C_COMPILER}
        -DHOST_BUILD_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/codegen_compile.cmake
)

set(DOLRECOMP_C_TEST_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/c_execute_generated.c)
add_custom_command(
    OUTPUT ${DOLRECOMP_C_TEST_SOURCE}
    COMMAND test_codegen_emit ${DOLRECOMP_C_TEST_SOURCE}
    DEPENDS test_codegen_emit
    VERBATIM
)
add_executable(test_c_execute
    tests/test_c_execute.c
    ${DOLRECOMP_C_TEST_SOURCE}
)
target_link_libraries(test_c_execute PRIVATE dr_cpu)
add_test(NAME c_execute COMMAND test_c_execute)

add_executable(test_fpscr tests/test_fpscr.c)
target_link_libraries(test_fpscr PRIVATE dr_cpu)
add_test(NAME fpscr COMMAND test_fpscr)

add_executable(test_float_semantics tests/test_float_semantics.c)
target_link_libraries(test_float_semantics PRIVATE dr_cpu)
add_test(NAME float_semantics COMMAND test_float_semantics)

add_executable(test_dolir tests/test_dolir.c)
target_link_libraries(test_dolir PRIVATE dr_ir)
add_test(NAME dolir COMMAND test_dolir)

add_executable(test_dolvm tests/test_dolvm.c)
target_link_libraries(test_dolvm PRIVATE dr_vm_backend dr_vm dr_ir)
add_test(NAME dolvm COMMAND test_dolvm)

# The same programs again with computed goto forced off, so the portable switch
# dispatch stays exercised on compilers that would have used threading.
# Every opcode the decoder knows, run through the C backend and the interpreter
# on identical state. The C backend is the shipping one, so it defines correct.
add_executable(test_dolvm_diff_gen tests/test_dolvm_diff_gen.c)
target_link_libraries(test_dolvm_diff_gen PRIVATE dr_backend dr_frontend dr_ir)

set(DOLVM_DIFF_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/dolvm_diff_generated.c)
add_custom_command(
    OUTPUT ${DOLVM_DIFF_SOURCE}
    COMMAND test_dolvm_diff_gen ${DOLVM_DIFF_SOURCE}
    DEPENDS test_dolvm_diff_gen
    VERBATIM
)
add_executable(test_dolvm_diff tests/test_dolvm_diff.c ${DOLVM_DIFF_SOURCE})
target_link_libraries(test_dolvm_diff PRIVATE dr_vm_backend dr_vm dr_ir dr_cpu)
add_test(NAME dolvm_diff COMMAND test_dolvm_diff)
add_test(NAME dolvm_diff_plain COMMAND test_dolvm_diff --no-home-state)
# Same comparison with the register file poisoned, which turns a read of a
# register the bytecode never wrote from luck into a failure.
add_test(NAME dolvm_diff_poison COMMAND test_dolvm_diff)
set_tests_properties(dolvm_diff_poison PROPERTIES
    ENVIRONMENT "DOLVM_POISON_REGS=1;DOLVM_POISON_BYTE=0xA0")
add_test(NAME dolvm_diff_poison_zero COMMAND test_dolvm_diff)
set_tests_properties(dolvm_diff_poison_zero PROPERTIES
    ENVIRONMENT "DOLVM_POISON_REGS=1;DOLVM_POISON_BYTE=0x00")

add_executable(test_dolvm_switch
    tests/test_dolvm.c
    src/vm/dolvm_interp.c
    src/vm/dolvm_module.c
    src/vm/dolvm_hle_native.c
)
target_compile_definitions(test_dolvm_switch PRIVATE DOLVM_FORCE_SWITCH=1)
target_link_libraries(test_dolvm_switch PRIVATE dr_vm_backend dr_ir dr_cpu)
add_test(NAME dolvm_switch COMMAND test_dolvm_switch)

if(DOLRECOMP_ENABLE_LLVM)
    add_executable(test_llvm_backend tests/test_llvm_backend.cpp)
    target_link_libraries(test_llvm_backend PRIVATE dr_llvm)
    add_test(NAME llvm_backend COMMAND test_llvm_backend
             ${CMAKE_CURRENT_BINARY_DIR}/dolir_test.o
             ${CMAKE_CURRENT_BINARY_DIR}/dolir_test.ll)
    if(APPLE)
        add_test(NAME llvm_backend_ios COMMAND test_llvm_backend
                 ${CMAKE_CURRENT_BINARY_DIR}/dolir_ios_test.o
                 ${CMAKE_CURRENT_BINARY_DIR}/dolir_ios_test.ll
                 arm64-apple-ios17.0)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME llvm_ios_helper_abi
                 COMMAND Python3::Interpreter
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_llvm_ios_helper_abi.py
                   --emitter-ir ${CMAKE_CURRENT_BINARY_DIR}/dolir_ios_test.ll
                   --fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/llvm_ios_helper_abi.c
                   --gxruntime-include ${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime/include)
        set_tests_properties(llvm_ios_helper_abi PROPERTIES
            DEPENDS llvm_backend_ios)
    endif()
    set(DOLRECOMP_TEST_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/dolir_execute.o)
    set(DOLRECOMP_TEST_IR ${CMAKE_CURRENT_BINARY_DIR}/dolir_execute.ll)
    add_custom_command(
        OUTPUT ${DOLRECOMP_TEST_OBJECT} ${DOLRECOMP_TEST_IR}
        COMMAND test_llvm_backend ${DOLRECOMP_TEST_OBJECT} ${DOLRECOMP_TEST_IR}
        DEPENDS test_llvm_backend
        VERBATIM
    )
    set_source_files_properties(${DOLRECOMP_TEST_OBJECT}
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_executable(test_llvm_execute tests/test_llvm_execute.c ${DOLRECOMP_TEST_OBJECT})
    target_link_libraries(test_llvm_execute PRIVATE dr_cpu)
    add_test(NAME llvm_execute COMMAND test_llvm_execute)
    add_executable(test_llvm_pipeline tests/test_llvm_pipeline.c)
    target_include_directories(test_llvm_pipeline PRIVATE ${DOLRECOMP_SRC})
    add_test(NAME llvm_pipeline COMMAND test_llvm_pipeline
             $<TARGET_FILE:dolrecomp>
             ${CMAKE_CURRENT_BINARY_DIR}/llvm_pipeline)
    if(APPLE)
        add_test(NAME llvm_pipeline_ios COMMAND test_llvm_pipeline
                 $<TARGET_FILE:dolrecomp>
                 ${CMAKE_CURRENT_BINARY_DIR}/llvm_pipeline_ios)
        set_tests_properties(llvm_pipeline_ios PROPERTIES
            ENVIRONMENT "DOLRECOMP_LLVM_TARGET=arm64-apple-ios17.0;DOLRECOMP_LLVM_CACHE=off")
    endif()
endif()
