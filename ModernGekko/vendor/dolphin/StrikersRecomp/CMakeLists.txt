cmake_minimum_required(VERSION 3.16)
project(StrikersRecomp C)

# ---------------------------------------------------------------------------
# Build type
#
# The generated chunk files are large switch-heavy translation units. They
# build fastest at low optimization, which is the right default for the
# booting-runtime milestone. Override with -DCMAKE_BUILD_TYPE=Release for an
# optimized build.
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(STRIKERSRECOMP_ENABLE_AURORA
       "Build with the Aurora graphics/input/audio backend" ON)
# The browser build: emcc, GXRuntime::web instead of GXRuntime::aurora, and the
# non-blocking entry point in runtime/host/main_web.c. Same generated code, same
# host policy files -- only the backend and the run loop differ.
option(STRIKERSRECOMP_ENABLE_WEB
       "Build for the browser (emscripten + WebGPU) instead of Aurora" OFF)
if(STRIKERSRECOMP_ENABLE_WEB)
    set(STRIKERSRECOMP_ENABLE_AURORA OFF CACHE BOOL "" FORCE)
    set(GXRUNTIME_ENABLE_WEB ON CACHE BOOL "" FORCE)
endif()
set(STRIKERSRECOMP_GXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/../GXRuntime"
    CACHE PATH "Path to a GXRuntime checkout")
set(STRIKERSRECOMP_AURORA_DIR "${STRIKERSRECOMP_GXRUNTIME_DIR}/graphics/aurora"
    CACHE PATH "Path to the GXRuntime-owned Aurora fork (GXRuntime/graphics/aurora)")

if(NOT EXISTS "${STRIKERSRECOMP_GXRUNTIME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "GXRuntime not found at ${STRIKERSRECOMP_GXRUNTIME_DIR}. Set "
        "-DSTRIKERSRECOMP_GXRUNTIME_DIR=<path>.")
endif()

set(GXRUNTIME_ENABLE_AURORA ${STRIKERSRECOMP_ENABLE_AURORA}
    CACHE BOOL "Build GXRuntime's Aurora backend" FORCE)
set(GXRUNTIME_AURORA_DIR "${STRIKERSRECOMP_AURORA_DIR}"
    CACHE PATH "Path to a GXRuntime-owned Aurora recomp-mode checkout, or a patch-applied Aurora checkout" FORCE)
add_subdirectory("${STRIKERSRECOMP_GXRUNTIME_DIR}"
                 "${CMAKE_BINARY_DIR}/GXRuntime")

# Where `tools/generate.py` placed the recompiled output.
set(STRIKERSRECOMP_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated"
    CACHE PATH "Directory containing DolRecomp output (generated.h, generated.c, chunks/)")

if(NOT EXISTS "${STRIKERSRECOMP_GENERATED_DIR}/generated.h")
    message(FATAL_ERROR
        "Recompiled output not found at ${STRIKERSRECOMP_GENERATED_DIR}.\n"
        "Run the generator first, e.g.:\n"
        "  python3 tools/generate.py --iso /path/to/strikers.iso")
endif()

# Constant-time chunk dispatch. Either tools/fix_generated.py rewrote the
# linear range chain (leaving its marker comment), or DolRecomp's emitter
# already emits indexed dispatch itself -- this tree's emitter does, as
# `dolrecomp_find_original` with a static chunk_functions[] table. Accept both.
file(STRINGS "${STRIKERSRECOMP_GENERATED_DIR}/generated.h"
     STRIKERSRECOMP_FAST_DISPATCH_MARKER
     REGEX "DolRecomp constant-time chunk dispatch|static const DolRecompFunction chunk_functions"
     LIMIT_COUNT 1)
if(NOT STRIKERSRECOMP_FAST_DISPATCH_MARKER)
    message(FATAL_ERROR
        "Generated output still uses DolRecomp's linear chunk dispatcher.\n"
        "Run: python3 tools/fix_generated.py")
endif()

file(GLOB STRIKERSRECOMP_CHUNKS CONFIGURE_DEPENDS
     "${STRIKERSRECOMP_GENERATED_DIR}/chunks/*.c")
list(LENGTH STRIKERSRECOMP_CHUNKS STRIKERSRECOMP_CHUNK_COUNT)
message(STATUS "StrikersRecomp: ${STRIKERSRECOMP_CHUNK_COUNT} recompiled chunk(s)")

if(STRIKERSRECOMP_ENABLE_WEB)
    set(STRIKERSRECOMP_ENTRY runtime/host/main_web.c)
else()
    set(STRIKERSRECOMP_ENTRY runtime/host/main.c)
endif()

add_executable(StrikersRecomp
    runtime/host/mmio.c
    runtime/host/audio.c
    runtime/host/interrupt.c
    runtime/host/hle.c
    runtime/host/hle_input.c
    runtime/host/hle_physics.c
    runtime/host/sdk_map.c
    ${STRIKERSRECOMP_ENTRY}
    "${STRIKERSRECOMP_GENERATED_DIR}/generated.c"
    ${STRIKERSRECOMP_CHUNKS}
)

if(NOT EXISTS "${STRIKERSRECOMP_GENERATED_DIR}/sdk_symbols.inc")
    message(FATAL_ERROR
        "SDK intercept table not found at ${STRIKERSRECOMP_GENERATED_DIR}/sdk_symbols.inc.\n"
        "Generate it from a smstrikers-decomp checkout:\n"
        "  python3 tools/symbols.py --decomp /path/to/smstrikers-decomp")
endif()

# - runtime/        : resolves "core/cpu.h" (used by the generated header) and
#                     "host/*.h" (used by main.c)
# - generated dir   : resolves "generated.h" (included by main.c)
target_include_directories(StrikersRecomp PRIVATE
    "${CMAKE_SOURCE_DIR}/runtime"
    "${STRIKERSRECOMP_GENERATED_DIR}"
)
target_link_libraries(StrikersRecomp PRIVATE GXRuntime::runtime)

include(CheckLibraryExists)
check_library_exists(m sqrt "" STRIKERSRECOMP_HAVE_LIBM)
if(STRIKERSRECOMP_HAVE_LIBM)
    target_link_libraries(StrikersRecomp PRIVATE m)
endif()

if(STRIKERSRECOMP_ENABLE_WEB)
    enable_language(CXX)
    target_link_libraries(StrikersRecomp PRIVATE GXRuntime::web)
    target_compile_features(StrikersRecomp PRIVATE cxx_std_20)
    # HAS_BACKEND gates every part of the host policy that needs a graphics
    # backend at all -- the GXBegin/GXCallDisplayList/GXSetArray intercepts and
    # the GXCopyDisp/GXLoadTexObj/VIConfigure notifies. It used to be spelled
    # STRIKERSRECOMP_AURORA, which silently left the browser build with no
    # present, no textures and no display lists.
    target_compile_definitions(StrikersRecomp PRIVATE
        STRIKERSRECOMP_WEB=1 STRIKERSRECOMP_HAS_BACKEND=1)
    set_target_properties(StrikersRecomp PROPERTIES
        LINKER_LANGUAGE CXX
        OUTPUT_NAME "strikers"
        SUFFIX ".js")
    # MODULARIZE so the page controls when the module instantiates; INVOKE_RUN=0
    # because boot is explicit (the user supplies a disc first). ALLOW_MEMORY_
    # GROWTH because guest RAM (24 MB MEM1 + 16 MB ARAM) plus the frame arenas
    # exceed the default heap.
    # Binaryen's wasm-opt over a ~30 MB module costs minutes and LLVM has
    # already optimised each translation unit (experiments/wasm/README.md
    # measured 373 s on a 29 MB link). Link at -O1 for iteration; turn this OFF
    # for a shipping build and measure whether the extra pass pays.
    option(STRIKERSRECOMP_WEB_FAST_LINK
           "Skip Binaryen's heavy link-time pass (much faster rebuilds)" ON)
    if(STRIKERSRECOMP_WEB_FAST_LINK)
        target_link_options(StrikersRecomp PRIVATE "-O1")
    endif()
    target_link_options(StrikersRecomp PRIVATE
        "-sENVIRONMENT=web,worker"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=StrikersWeb"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=128MB"
        "-sMAXIMUM_MEMORY=2GB"
        "-sSTACK_SIZE=4MB"
        "-sINVOKE_RUN=0"
        "-sEXIT_RUNTIME=0"
        "-sFORCE_FILESYSTEM=1"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','HEAPU8','HEAPU16','HEAPU32','HEAPF32','HEAP16','FS','ENV','callMain']"
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
    )
elseif(STRIKERSRECOMP_ENABLE_AURORA)
    enable_language(CXX)
    target_link_libraries(StrikersRecomp PRIVATE GXRuntime::aurora)
    target_compile_features(StrikersRecomp PRIVATE cxx_std_20)
    target_compile_definitions(StrikersRecomp PRIVATE
        STRIKERSRECOMP_AURORA=1 STRIKERSRECOMP_HAS_BACKEND=1)
    set_target_properties(StrikersRecomp PROPERTIES LINKER_LANGUAGE CXX)
endif()
