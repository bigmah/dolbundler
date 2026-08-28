cmake_minimum_required(VERSION 3.16)
project(recompcore_module C)

# Per-game native module for RecompCore: generated chunks + DolRecomp CPU
# runtime + ABI export glue, no other host dependencies.
#
# Configure with:
#   cmake -B build -DGAME_ID=G4QE01 \
#         -DGENERATED_DIR=/path/to/generated \
#         -DGXRUNTIME_DIR=/path/to/GXRuntime

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT DEFINED GAME_ID)
  set(GAME_ID "" CACHE STRING "Six-character disc ID, e.g. G4QE01")
endif()
if(NOT GAME_ID)
  message(FATAL_ERROR "Set -DGAME_ID=<disc id> (e.g. -DGAME_ID=G4QE01)")
endif()
if(NOT DEFINED GENERATED_DIR)
  set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH
      "DolRecomp output directory (generated.h, generated_smc.txt, main.dol, chunks/)")
endif()
if(NOT EXISTS "${GENERATED_DIR}")
  message(FATAL_ERROR "Generated directory not found at ${GENERATED_DIR}. Set -DGENERATED_DIR=<path to DolRecomp output>")
endif()
foreach(REQUIRED_GENERATED_FILE generated.h generated_smc.txt main.dol)
  if(NOT EXISTS "${GENERATED_DIR}/${REQUIRED_GENERATED_FILE}")
    message(FATAL_ERROR
      "DolRecomp output is incomplete: ${GENERATED_DIR}/${REQUIRED_GENERATED_FILE} is missing")
  endif()
endforeach()
if(NOT DEFINED GXRUNTIME_DIR)
  set(GXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime" CACHE PATH
      "GXRuntime checkout")
endif()
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found at ${GXRUNTIME_DIR}. Set -DGXRUNTIME_DIR=<path to GXRuntime>")
endif()
if(NOT DEFINED CHASSIS_ABI_DIR)
  set(CHASSIS_ABI_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Core/Core/PowerPC/StaticRecomp"
      CACHE PATH "RecompCore StaticRecomp ABI directory")
endif()

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(READ "${GENERATED_DIR}/generated.h" GENERATED_HEADER_TEXT)
set(MODULE_SYMBOL_PREFIX "")
string(REGEX MATCH
  "#define DOLRECOMP_NATIVE_SYMBOL_PREFIX \"[^\"]*\""
  MODULE_SYMBOL_PREFIX_LINE "${GENERATED_HEADER_TEXT}")
if(MODULE_SYMBOL_PREFIX_LINE)
  string(REGEX REPLACE
    "^#define DOLRECOMP_NATIVE_SYMBOL_PREFIX \"([^\"]*)\"$" "\\1"
    MODULE_SYMBOL_PREFIX "${MODULE_SYMBOL_PREFIX_LINE}")
endif()
if(MODULE_SYMBOL_PREFIX AND
   NOT MODULE_SYMBOL_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
  message(FATAL_ERROR
    "Invalid generated symbol prefix: ${MODULE_SYMBOL_PREFIX}")
endif()
if(GENERATED_HEADER_TEXT MATCHES "DOLRECOMP_BACKEND_LLVM")
  foreach(REQUIRED_METADATA
          DOLRECOMP_NATIVE_BACKEND_NAME
          DOLRECOMP_NATIVE_TARGET_TRIPLE
          DOLRECOMP_NATIVE_TARGET_CPU
          DOLRECOMP_NATIVE_LLVM_VERSION
          DOLRECOMP_NATIVE_CODEGEN_FINGERPRINT
          DOLRECOMP_NATIVE_BUILD_ID
          DOLRECOMP_NATIVE_STATE_LAYOUT_HASH)
    if(NOT GENERATED_HEADER_TEXT MATCHES "#define ${REQUIRED_METADATA} ")
      message(FATAL_ERROR
        "DolRecomp LLVM metadata is incomplete: ${REQUIRED_METADATA} is missing")
    endif()
  endforeach()
  if(NOT GENERATED_HEADER_TEXT MATCHES
      "#define DOLRECOMP_NATIVE_BACKEND_NAME \"llvm\"")
    message(FATAL_ERROR "Generated object manifest does not identify the LLVM backend")
  endif()
  set(LLVM_SYMBOL_AUDIT_OPTIONS)
  set(RECOMPCORE_MODULE_PGO_GENERATE OFF)
  if(GENERATED_HEADER_TEXT MATCHES "pgo=gen")
    # Instrumented LLVM objects intentionally reference compiler-rt's profile
    # runtime. Keep the normal object audit closed over all other symbols and
    # propagate the driver option that links the runtime into the final app.
    list(APPEND LLVM_SYMBOL_AUDIT_OPTIONS --allow-profile-runtime)
    set(RECOMPCORE_MODULE_PGO_GENERATE ON)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
     NOT CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
    set(EXPECTED_LLVM_TRIPLE "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT GENERATED_HEADER_TEXT MATCHES
       "#define DOLRECOMP_NATIVE_TARGET_TRIPLE \"${EXPECTED_LLVM_TRIPLE}\"")
      message(FATAL_ERROR
        "Generated LLVM target must be ${EXPECTED_LLVM_TRIPLE} for this iPhoneOS build")
    endif()
    set(RECOMPCORE_EXPECTED_LLVM_CPU "apple-a16" CACHE STRING
        "Required LLVM CPU for embedded iPhoneOS objects")
    if(NOT GENERATED_HEADER_TEXT MATCHES
       "#define DOLRECOMP_NATIVE_TARGET_CPU \"${RECOMPCORE_EXPECTED_LLVM_CPU}\"")
      message(FATAL_ERROR
        "Generated LLVM CPU does not match ${RECOMPCORE_EXPECTED_LLVM_CPU}")
    endif()
  endif()

  include(CheckCSourceCompiles)
  set(_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
  set(CMAKE_REQUIRED_INCLUDES
      "${GENERATED_DIR};${GXRUNTIME_DIR}/include")
  check_c_source_compiles(
    "#include \"generated.h\"\n#include \"core/native_state_layout.h\"\nint main(void) { return 0; }"
    RECOMPCORE_NATIVE_LAYOUT_MATCHES)
  set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
  if(NOT RECOMPCORE_NATIVE_LAYOUT_MATCHES)
    message(FATAL_ERROR
      "Generated LLVM CPUState offsets do not match the target GXRuntime layout")
  endif()

  set(GENERATED_MANIFEST)
  foreach(MANIFEST_CANDIDATE
          "${GENERATED_DIR}/${GAME_ID}.c"
          "${GENERATED_DIR}/generated.c")
    if(EXISTS "${MANIFEST_CANDIDATE}")
      set(GENERATED_MANIFEST "${MANIFEST_CANDIDATE}")
      break()
    endif()
  endforeach()
  if(NOT GENERATED_MANIFEST)
    message(FATAL_ERROR "DolRecomp LLVM object manifest is missing")
  endif()
  file(STRINGS "${GENERATED_MANIFEST}" CHUNK_OBJECT_LINES
       REGEX "^// object: ")
  foreach(CHUNK_OBJECT_LINE IN LISTS CHUNK_OBJECT_LINES)
    string(REGEX REPLACE "^// object: " "" CHUNK_OBJECT
           "${CHUNK_OBJECT_LINE}")
    list(APPEND CHUNK_INPUTS "${GENERATED_DIR}/${CHUNK_OBJECT}")
  endforeach()
  foreach(CHUNK_INPUT IN LISTS CHUNK_INPUTS)
    if(NOT EXISTS "${CHUNK_INPUT}")
      message(FATAL_ERROR "DolRecomp LLVM object is missing: ${CHUNK_INPUT}")
    endif()
  endforeach()
  set(LLVM_OBJECT_LIST "${CMAKE_CURRENT_BINARY_DIR}/llvm_objects.txt")
  list(JOIN CHUNK_INPUTS "\n" LLVM_OBJECT_LIST_TEXT)
  file(WRITE "${LLVM_OBJECT_LIST}" "${LLVM_OBJECT_LIST_TEXT}\n")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/verify_llvm_objects.py"
            --nm "${CMAKE_NM}"
            --cpu-header "${GXRUNTIME_DIR}/include/core/cpu.h"
            --symbol-prefix "${MODULE_SYMBOL_PREFIX}"
            --object-list "${LLVM_OBJECT_LIST}"
            ${LLVM_SYMBOL_AUDIT_OPTIONS}
    RESULT_VARIABLE LLVM_SYMBOL_AUDIT_RESULT
    OUTPUT_VARIABLE LLVM_SYMBOL_AUDIT_OUTPUT
    ERROR_VARIABLE LLVM_SYMBOL_AUDIT_ERROR)
  if(NOT LLVM_SYMBOL_AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR
      "LLVM object symbol audit failed:\n${LLVM_SYMBOL_AUDIT_OUTPUT}${LLVM_SYMBOL_AUDIT_ERROR}")
  endif()
  message(STATUS "${LLVM_SYMBOL_AUDIT_OUTPUT}")
  set_source_files_properties(${CHUNK_INPUTS} PROPERTIES EXTERNAL_OBJECT TRUE)
else()
  file(GLOB CHUNK_INPUTS CONFIGURE_DEPENDS "${GENERATED_DIR}/chunks/*.c")
endif()
if(NOT CHUNK_INPUTS)
  message(FATAL_ERROR "DolRecomp output has no generated chunks in ${GENERATED_DIR}/chunks")
endif()

option(RECOMPCORE_MODULE_EMBEDDED
       "Build a static module that shares the host's GXRuntime CPU helpers" OFF)

if(RECOMPCORE_MODULE_EMBEDDED)
  set(MODULE_LIBRARY_TYPE STATIC)
  # moderngekko_dolvm already provides the common CPU helpers in an embedded
  # host. The exception implementation is the only native-module source it
  # does not compile itself.
  set(MODULE_RUNTIME_SOURCES
      "${GXRUNTIME_DIR}/src/core/cpu_exception.c")
else()
  set(MODULE_LIBRARY_TYPE SHARED)
  set(MODULE_RUNTIME_SOURCES
      "${GXRUNTIME_DIR}/src/core/cpu.c"
      "${GXRUNTIME_DIR}/src/core/cpu_exception.c"
      "${GXRUNTIME_DIR}/src/core/cpu_interpreter.c"
      "${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c"
      "${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c"
      "${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c")
endif()

add_library(${MODULE_NAME} ${MODULE_LIBRARY_TYPE}
  module_export.c
  "${MODULE_TABLES}"
  ${MODULE_RUNTIME_SOURCES}
  ${CHUNK_INPUTS})

if(RECOMPCORE_MODULE_PGO_GENERATE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "LLVM profile-generation objects require a Clang final-link driver")
  endif()
  if(RECOMPCORE_MODULE_EMBEDDED)
    target_link_options(${MODULE_NAME} INTERFACE -fprofile-instr-generate)
    target_compile_definitions(${MODULE_NAME} INTERFACE
      DOLBUNDLER_LLVM_PGO_GENERATE=1)
  else()
    target_link_options(${MODULE_NAME} PRIVATE -fprofile-instr-generate)
  endif()
endif()

set_target_properties(${MODULE_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${MODULE_NAME}"
  UNITY_BUILD OFF
  C_VISIBILITY_PRESET hidden)
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")
if(MODULE_SYMBOL_PREFIX)
  target_compile_definitions(${MODULE_NAME} PRIVATE
    MODULE_SYMBOL_PREFIX=${MODULE_SYMBOL_PREFIX})
endif()

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${GXRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT WIN32)
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()
if(UNIX AND NOT APPLE)
  set(MODULE_EXPORTS "${CMAKE_CURRENT_BINARY_DIR}/module.exports")
  file(WRITE "${MODULE_EXPORTS}"
    "{\n  global:\n    ${MODULE_SYMBOL_PREFIX}staticrecomp_get_module;\n"
    "    ppc_set_mem_write_journal;\n  local:\n    *;\n};\n")
  target_link_options(${MODULE_NAME} PRIVATE
    "LINKER:--version-script=${MODULE_EXPORTS}")
  set_property(TARGET ${MODULE_NAME} APPEND PROPERTY LINK_DEPENDS
    "${MODULE_EXPORTS}")
endif()

option(RECOMPCORE_MODULE_ENABLE_IPO "Enable Clang module interprocedural optimization when supported" ON)
if(NOT DEFINED RECOMPCORE_MODULE_OPT_LEVEL)
  set(RECOMPCORE_MODULE_OPT_LEVEL "2" CACHE STRING
      "Optimization level for generated module translation units (0, 1, 2, or 3)")
endif()
# s and z are here because a whole game's recompiled code is the binary: on the
# wasm target the module is 91.6 MB of a 96 MB module, so what it costs to
# compile for size instead of speed is a question worth being able to ask.
set_property(CACHE RECOMPCORE_MODULE_OPT_LEVEL PROPERTY STRINGS 0 1 2 3 s z)
if(NOT RECOMPCORE_MODULE_OPT_LEVEL MATCHES "^[0-3sz]$")
  message(FATAL_ERROR
    "RECOMPCORE_MODULE_OPT_LEVEL must be one of 0, 1, 2, 3, s, or z")
endif()
if(RECOMPCORE_MODULE_ENABLE_IPO AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  include(CheckIPOSupported)
  # moderngekko-port configures with CMAKE_NINJA_FORCE_RESPONSE_FILE=1, which the
  # module needs: it links thousands of chunk objects and the command line does
  # not fit in ARG_MAX. But check_ipo_supported's try-compile inherits the
  # environment, and its static-library step then runs `ar qc libfoo.a @foo.rsp`
  # -- Apple's /usr/bin/ar has no @file support, so the check fails with
  # "ar: @CMakeFiles/foo.rsp: No such file or directory" and IPO is silently
  # disabled. The manifest still records -flto=thin. Every module built this way
  # had NO cross-translation-unit inlining. Scope the variable off for the check
  # only, then restore it before the generator reads it.
  set(_module_forced_rsp "$ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE}")
  unset(ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE})
  check_ipo_supported(RESULT RECOMPCORE_MODULE_IPO_SUPPORTED
                      OUTPUT RECOMPCORE_MODULE_IPO_ERROR LANGUAGES C)
  if(_module_forced_rsp)
    set(ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE} "${_module_forced_rsp}")
  endif()
  if(RECOMPCORE_MODULE_IPO_SUPPORTED)
    set_property(TARGET ${MODULE_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(STATUS "Module IPO disabled: ${RECOMPCORE_MODULE_IPO_ERROR}")
  endif()
endif()

if(MSVC)
  if(RECOMPCORE_MODULE_OPT_LEVEL STREQUAL "0")
    target_compile_options(${MODULE_NAME} PRIVATE /Od /fp:strict)
  elseif(RECOMPCORE_MODULE_OPT_LEVEL MATCHES "^[sz]$")
    target_compile_options(${MODULE_NAME} PRIVATE /O1 /fp:strict)
  else()
    target_compile_options(${MODULE_NAME} PRIVATE /O2 /fp:strict)
  endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(${MODULE_NAME} PRIVATE
    -O${RECOMPCORE_MODULE_OPT_LEVEL} -ffp-contract=off -fno-fast-math)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(${MODULE_NAME} PRIVATE
    -O${RECOMPCORE_MODULE_OPT_LEVEL} -ffp-contract=off -fno-fast-math)
endif()
