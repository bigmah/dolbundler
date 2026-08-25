cmake_minimum_required(VERSION 3.20)
if(APPLE)
    project(ModernGekko LANGUAGES C CXX OBJCXX)
else()
    project(ModernGekko LANGUAGES C CXX)
endif()

include(CTest)
include(CheckIncludeFileCXX)

if(WIN32)
    set(MODERNGEKKO_DYNAMIC_MODULES_DEFAULT ON)
else()
    check_include_file_cxx(dlfcn.h MODERNGEKKO_HAS_DLFCN)
    set(MODERNGEKKO_DYNAMIC_MODULES_DEFAULT ${MODERNGEKKO_HAS_DLFCN})
endif()
option(MODERNGEKKO_ENABLE_DYNAMIC_MODULES
       "Load game modules through the host dynamic-library API"
       ${MODERNGEKKO_DYNAMIC_MODULES_DEFAULT})

option(MODERNGEKKO_DOLVM_PGO_GENERATE
    "Instrument the interpreter so a run writes a PGO profile" OFF)
set(MODERNGEKKO_DOLVM_PGO_PROFILE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dolvm/dolvm.profdata" CACHE FILEPATH
    "llvm-profdata file to build the interpreter against; empty to disable")
option(MODERNGEKKO_DOLVM_SAMPLE
    "Sample the interpreter and attribute host time to opcodes (implies BENCH and PROFILE)" OFF)
option(MODERNGEKKO_DOLVM_BENCH
    "Stop once DOLVM_BENCH_CYCLES guest cycles have been interpreted, reporting the wall time" OFF)
option(MODERNGEKKO_DOLVM_PROFILE
    "Count executed DolVM opcodes and print the histogram when a bytecode module closes" OFF)
option(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME
       "Build the RecompCore/Dolphin production chassis" ON)
option(MODERNGEKKO_ENABLE_DOLPHIN_TESTS
       "Build the imported Dolphin unit-test targets" OFF)
option(MODERNGEKKO_ENABLE_DISC_TOOL
       "Build Dolphin's disc conversion and verification tool" OFF)
set(MODERNGEKKO_FRONTEND_NAME "ModernGekko" CACHE STRING
    "Branded frontend display name")
set(MODERNGEKKO_LAUNCHER_OUTPUT_NAME "ModernGekko" CACHE STRING
    "Launcher executable filename without a platform suffix")
set(MODERNGEKKO_RUNNER_OUTPUT_NAME "moderngekko-run" CACHE STRING
    "Runner executable filename without a platform suffix")
set(MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko" CACHE STRING
    "Frontend user directory name")
set(MODERNGEKKO_REQUIRED_DISC_ID "" CACHE STRING
    "Required six-character disc ID for branded builds")
set(MODERNGEKKO_REQUIRED_DOL_SHA256 "" CACHE STRING
    "Required extracted main DOL SHA-256")
set(MODERNGEKKO_REQUIRED_REL_SHA256 "" CACHE STRING
    "Required extracted main REL SHA-256")
set(MODERNGEKKO_REQUIRED_ASSETS_SHA256 "" CACHE STRING
    "Required extracted files tree SHA-256")
set(MODERNGEKKO_DEFAULT_WINDOW_TITLE "" CACHE STRING
    "Fixed game window title for branded builds")
option(MODERNGEKKO_PORTABLE_DEFAULT_GAME
       "Read and write default-game.txt beside branded executables" OFF)
option(MODERNGEKKO_GAMECUBE_CONTROLLERS
       "Generate GameCube controller profiles instead of Wii Remote profiles" OFF)
set(MODERNGEKKO_LOG_FILENAME "ModernGekko.log" CACHE STRING
    "Runner log filename")
set(MODERNGEKKO_DISC_PREPARER_OUTPUT_NAME "" CACHE STRING
    "Optional adjacent disc preparation executable")
set(MODERNGEKKO_DOL_PATCH_MANIFEST "" CACHE FILEPATH
    "Optional expected-word DOL patch manifest bundled with the frontend")
set(MODERNGEKKO_GAME_SETTINGS_OVERRIDE "" CACHE FILEPATH
    "Optional game INI copied over the bundled Dolphin settings")
set(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID "" CACHE STRING
    "Source disc ID accepted by the optional disc preparer")
option(MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
       "Place the branded user directory in Documents" OFF)
set(MODERNGEKKO_FRONTEND_SDK "" CACHE PATH
    "Optional non-root sysroot containing X11 and audio development packages")

if(MODERNGEKKO_FRONTEND_SDK)
    set(_moderngekko_sdk_usr "${MODERNGEKKO_FRONTEND_SDK}/usr")
    set(_moderngekko_sdk_lib "${_moderngekko_sdk_usr}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    if(NOT IS_DIRECTORY "${_moderngekko_sdk_usr}/include" OR
       NOT IS_DIRECTORY "${_moderngekko_sdk_lib}")
        message(FATAL_ERROR
            "MODERNGEKKO_FRONTEND_SDK is not a usable sysroot: ${MODERNGEKKO_FRONTEND_SDK}")
    endif()

    list(PREPEND CMAKE_PREFIX_PATH "${_moderngekko_sdk_usr}")
    list(PREPEND CMAKE_INCLUDE_PATH "${_moderngekko_sdk_usr}/include")
    list(PREPEND CMAKE_LIBRARY_PATH "${_moderngekko_sdk_lib}")
    link_directories("${_moderngekko_sdk_lib}")

    set(_moderngekko_pkgconfig
        "${_moderngekko_sdk_lib}/pkgconfig:${_moderngekko_sdk_usr}/share/pkgconfig")
    if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        string(APPEND _moderngekko_pkgconfig ":$ENV{PKG_CONFIG_PATH}")
    endif()
    set(ENV{PKG_CONFIG_PATH} "${_moderngekko_pkgconfig}")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MODERNGEKKO_FRONTEND_SDK}")

    # FindALSA does not use pkg-config on all supported CMake releases.
    if(EXISTS "${_moderngekko_sdk_lib}/libasound.so")
        set(ALSA_INCLUDE_DIR "${_moderngekko_sdk_usr}/include" CACHE PATH "" FORCE)
        set(ALSA_LIBRARY "${_moderngekko_sdk_lib}/libasound.so" CACHE FILEPATH "" FORCE)
    endif()
endif()

if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    set(MODERNGEKKO_ENABLE_IMGUI_LAUNCHER ON CACHE BOOL "" FORCE)
    set(ENABLE_QT OFF CACHE BOOL "" FORCE)
    set(ENABLE_NOGUI OFF CACHE BOOL "" FORCE)
    set(ENABLE_CLI_TOOL ${MODERNGEKKO_ENABLE_DISC_TOOL} CACHE BOOL "" FORCE)
    set(ENABLE_TESTS ${MODERNGEKKO_ENABLE_DOLPHIN_TESTS} CACHE BOOL "" FORCE)
    set(ENABLE_AUTOUPDATE OFF CACHE BOOL "" FORCE)
    set(ENABLE_ANALYTICS OFF CACHE BOOL "" FORCE)
    set(USE_RETRO_ACHIEVEMENTS OFF CACHE BOOL "" FORCE)
    set(USE_DISCORD_PRESENCE OFF CACHE BOOL "" FORCE)
    set(USE_MGBA OFF CACHE BOOL "" FORCE)
    set(USE_UPNP OFF CACHE BOOL "" FORCE)
    set(ENCODE_FRAMEDUMPS OFF CACHE BOOL "" FORCE)
    set(ENABLE_LLVM OFF CACHE BOOL "" FORCE)
    # Cubeb is Dolphin's only audio backend on macOS
    if(APPLE OR WIN32)
        set(ENABLE_CUBEB ON CACHE BOOL "" FORCE)
    else()
        set(ENABLE_CUBEB OFF CACHE BOOL "" FORCE)
    endif()
    set(ENABLE_HWDB OFF CACHE BOOL "" FORCE)
    set(ENABLE_EVDEV OFF CACHE BOOL "" FORCE)
    set(ENABLE_WAYLAND ON CACHE BOOL "" FORCE)
    set(ENABLE_EGL OFF CACHE BOOL "" FORCE)
    set(USE_SYSTEM_SDL3 OFF CACHE BOOL "" FORCE)
    # SDL bundles its own copy of the macOS hidapi backend
    if(APPLE)
        set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    endif()
    set(LINUX_LOCAL_DEV ON CACHE BOOL "" FORCE)
    set(DISTRIBUTOR "ModernGekko" CACHE STRING "" FORCE)
    add_subdirectory(vendor/dolphin EXCLUDE_FROM_ALL)
    target_sources(core PRIVATE
        vendor/dolphin/GXRuntime/src/core/cpu_exception.c)
    target_include_directories(core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include")
    if(MODERNGEKKO_FRONTEND_SDK)
        # These two imported targets historically rely on X11 headers living
        # in the compiler's default /usr/include search path.
        target_include_directories(common SYSTEM PRIVATE "${_moderngekko_sdk_usr}/include")
        if(TARGET videovulkan)
            target_include_directories(videovulkan SYSTEM PRIVATE
                "${_moderngekko_sdk_usr}/include")
        endif()
    endif()
else()
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/fmt/fmt EXCLUDE_FROM_ALL)
endif()

if(BUILD_TESTING)
    add_subdirectory(vendor/dolphin/DolRecomp)
else()
    add_subdirectory(vendor/dolphin/DolRecomp EXCLUDE_FROM_ALL)
endif()

add_library(moderngekko_abi INTERFACE)
add_library(ModernGekko::ABI ALIAS moderngekko_abi)

target_include_directories(moderngekko_abi
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include>"
        "$<INSTALL_INTERFACE:include>"
)

add_library(moderngekko_legacy STATIC
    src/audio/audio_system.cpp
    src/hardware/mmio_bus.cpp
    src/hardware/disc_interface.cpp
    src/hardware/processor_interface.cpp
    src/gpu/gx_command_processor.cpp
    src/gpu/gx_state_backend.cpp
    src/gpu/gx_vertex_loader.cpp
    src/gpu/gx_texture_decoder.cpp
    src/gpu/gx_pipeline.cpp
    src/input/controller.cpp
    src/memory/address_space.cpp
    src/runtime/module.cpp
    src/runtime/module_loader.cpp
    src/runtime/mod_loader.cpp
    src/runtime/game.cpp
    src/runtime/legacy_runtime.cpp
    src/timing/event_scheduler.cpp
    vendor/dolphin_legacy/Core/Boot/BootMemory.cpp
    vendor/dolphin_legacy/Core/Boot/DolReader.cpp
    vendor/dolphin_legacy/Core/PowerPC/Interpreter/IntegerBranch.cpp
    vendor/dolphin_legacy/Core/PowerPC/ConditionRegister.cpp
)
add_library(ModernGekko::LegacyRuntime ALIAS moderngekko_legacy)

# DolRecomp's interpreter half, compiled against the chassis's CPUState rather
# than the recompiler's own runtime copy of it. That is what lets a .dvm -- a
# recompilation lowered to bytecode instead of to machine code -- be dispatched
# through the same module ABI a native module uses.
#
# The shim include directory has to come before DolRecomp's own src/: both trees
# ship a "cpu/cpu.h" and a "common/types.h" under matching include guards, and
# the chassis needs GXRuntime's of each.
if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    add_library(moderngekko_dolvm STATIC
        src/runtime/dolvm/dolvm_bridge.c
        vendor/dolphin/DolRecomp/src/vm/dolvm_module.c
        vendor/dolphin/DolRecomp/src/vm/dolvm_interp.c
        vendor/dolphin/DolRecomp/src/vm/dolvm_hle_native.c
        vendor/dolphin/GXRuntime/src/core/cpu.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter_table.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter_float.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter_integer.c
    )
    target_include_directories(moderngekko_dolvm
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dolvm"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dolvm/shim"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/DolRecomp/src"
    )
    set_target_properties(moderngekko_dolvm PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_VISIBILITY_PRESET hidden
    )
    if(NOT MSVC)
        # The guest's arithmetic is the guest's; the host may not reassociate it.
        # -fno-stack-protector: the dispatch loop's frame holds a 2 KB register
        # file, so it gets a guard by default -- a load, a store and a compare
        # on a function called once per thirty guest instructions.
        #
        # -mcpu: the host's own core on a native build. A cross build for iOS
        # names the A16 -- the same ARMv8.6 core generation as the A17 Pro the
        # app is tested on, and `native` would quietly pick whatever Mac is
        # doing the building.
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(_dolvm_cpu -mcpu=apple-a16)
        else()
            set(_dolvm_cpu -mcpu=native)
        endif()
        target_compile_options(moderngekko_dolvm PRIVATE
            -ffp-contract=off -fno-fast-math ${_dolvm_cpu} -fno-stack-protector)
        # The interpreter is threaded code: every handler ends in its own
        # indirect branch, so each one gets its own branch-prediction history.
        # LLVM's tail merger undoes that -- it folds the 180 identical
        # `load op; load handler; branch` tails into one, and the tail
        # duplicator that would split them back up refuses once a block has
        # more than 16 predecessors. Raising the limit keeps one `br` per
        # handler, which is worth 3-7% of wall clock on real titles. CMake
        # de-duplicates repeated flags, so each `-mllvm` pair needs SHELL:.
        target_compile_options(moderngekko_dolvm PRIVATE
            "SHELL:-mllvm -tail-dup-pred-size=256"
            "SHELL:-mllvm -tail-dup-succ-size=256")
        # Link-time optimization over the interpreter and the cpu helpers it
        # calls: the paired-single arithmetic is a call into cpu_interpreter_
        # float.c per lane per instruction, and on a title that lives in
        # paired singles inlining it is worth 3%. The codegen then happens at
        # link, so the tail-duplication limits have to reach the linker too,
        # or the per-handler branches are lost again and everything regresses.
        target_compile_options(moderngekko_dolvm PRIVATE -flto)
        target_link_options(moderngekko_dolvm INTERFACE -flto
            "SHELL:-Wl,-mllvm,-tail-dup-pred-size=256"
            "SHELL:-Wl,-mllvm,-tail-dup-succ-size=256")
    endif()
    if(MODERNGEKKO_DOLVM_PROFILE)
        target_compile_definitions(moderngekko_dolvm PRIVATE DOLVM_PROFILE=1)
    endif()
    if(MODERNGEKKO_DOLVM_BENCH)
        target_compile_definitions(moderngekko_dolvm PRIVATE DOLVM_BENCH=1)
    endif()
    # The dispatch loop is one enormous function, and which handler follows
    # which is the whole question -- exactly what a compiler has to guess at and
    # a recorded profile does not have to. Worth 6-8% of wall clock, so the
    # profile ships with the tree: generating one needs a game, and the tree
    # ships none. Regenerate it after changing the interpreter, or the compiler
    # quietly falls back to guessing:
    #
    #   cmake -DMODERNGEKKO_DOLVM_PGO_GENERATE=ON -DMODERNGEKKO_DOLVM_BENCH=ON .
    #   cmake --build . --target moderngekko-run
    #   LLVM_PROFILE_FILE=<game>.profraw DOLVM_BENCH_CYCLES=6000000000 \
    #       ./moderngekko-run --game <extracted> --module <game>.dvm --headless \
    #       --graphics Null --audio "No Audio Output"
    #   xcrun llvm-profdata merge -output=src/runtime/dolvm/dolvm.profdata *.profraw
    if(MODERNGEKKO_DOLVM_PGO_GENERATE)
        target_compile_options(moderngekko_dolvm PUBLIC -fprofile-instr-generate)
        target_link_options(moderngekko_dolvm PUBLIC -fprofile-instr-generate)
        target_compile_definitions(moderngekko_dolvm PRIVATE DOLVM_PGO_GENERATE=1)
    elseif(MODERNGEKKO_DOLVM_PGO_PROFILE AND EXISTS "${MODERNGEKKO_DOLVM_PGO_PROFILE}")
        # A profile is tied to the format its llvm-profdata wrote, so a compiler
        # that cannot read this one has to build without it rather than fail.
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS
            "-fprofile-instr-use=${MODERNGEKKO_DOLVM_PGO_PROFILE}")
        check_c_source_compiles("int main(void){return 0;}"
            MODERNGEKKO_DOLVM_PGO_USABLE)
        unset(CMAKE_REQUIRED_FLAGS)
        if(MODERNGEKKO_DOLVM_PGO_USABLE)
            target_compile_options(moderngekko_dolvm PRIVATE
                "-fprofile-instr-use=${MODERNGEKKO_DOLVM_PGO_PROFILE}"
                # A profile older than the interpreter costs the speedup, not
                # correctness, so say so once at configure time rather than
                # shouting on every translation unit.
                -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
            # Every object in the library has to be built against the same
            # profile: LTO refuses to merge modules whose profile summaries
            # disagree, which is what an incremental build produces when the
            # profile changes and only the interpreter recompiles. A hash of
            # the profile in the compile definitions makes a new profile a
            # new set of flags, which every generator -- Xcode included, which
            # ignores OBJECT_DEPENDS -- treats as a reason to rebuild all of it.
            file(SHA256 "${MODERNGEKKO_DOLVM_PGO_PROFILE}" _dolvm_profile_hash)
            string(SUBSTRING "${_dolvm_profile_hash}" 0 16 _dolvm_profile_hash)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${MODERNGEKKO_DOLVM_PGO_PROFILE}")
            target_compile_definitions(moderngekko_dolvm PRIVATE
                "DOLVM_PGO_PROFILE_ID=0x${_dolvm_profile_hash}")
        else()
            message(STATUS
                "DolVM: this compiler cannot read the shipped PGO profile; "
                "building the interpreter without it")
        endif()
    endif()
    # Sampling deliberately does *not* turn the opcode counters on. Counting is
    # two global increments per opcode, which is enough to make every handler's
    # epilogue identical again -- LLVM's tail merger then folds them into one
    # shared indirect branch, and the profile that comes back describes a build
    # nobody ships. Turn MODERNGEKKO_DOLVM_PROFILE on as well when the executed
    # mix is what is wanted; leave it off when the question is where time goes.
    if(MODERNGEKKO_DOLVM_SAMPLE)
        target_compile_definitions(moderngekko_dolvm PRIVATE
            DOLVM_BENCH=1 DOLVM_SAMPLE=1)
    endif()
    if(NOT WIN32)
        target_link_libraries(moderngekko_dolvm PRIVATE m)
    endif()
endif()

add_library(moderngekko STATIC
    src/runtime/module.cpp
    src/runtime/module_loader.cpp
    src/runtime/mod_loader.cpp
    src/runtime/game.cpp
)
add_library(ModernGekko::Runtime ALIAS moderngekko)

add_library(moderngekko_dolphin_video STATIC
    src/gpu/dolphin_video_host.cpp
    src/gpu/dolphin_shader_compiler.cpp
    vendor/dolphin_legacy/VideoCommon/BPMemory.cpp
    vendor/dolphin_legacy/VideoCommon/XFMemory.cpp
    vendor/dolphin_legacy/VideoCommon/ShaderGenCommon.cpp
    vendor/dolphin_legacy/VideoCommon/LightingShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/PixelShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/VertexShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/GeometryShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/DriverDetails.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderCommon.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderPixel.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderVertex.cpp
)
if(APPLE)
    target_sources(moderngekko_dolphin_video PRIVATE
        vendor/dolphin_legacy/Common/CommonFuncsObjC.mm)
    target_link_libraries(moderngekko_dolphin_video PUBLIC "-framework Foundation")
endif()
add_library(ModernGekko::DolphinVideo ALIAS moderngekko_dolphin_video)
set_target_properties(moderngekko_dolphin_video PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(moderngekko_dolphin_video PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(moderngekko_dolphin_video PUBLIC /Zc:preprocessor)
    target_compile_definitions(moderngekko_dolphin_video PUBLIC UNICODE _UNICODE)
endif()
target_include_directories(moderngekko_dolphin_video
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
           "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
           "${CMAKE_CURRENT_SOURCE_DIR}/vendor/picojson"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/dolphin_shims"
)
target_link_libraries(moderngekko_dolphin_video PUBLIC fmt::fmt)

target_link_libraries(moderngekko_legacy PUBLIC ModernGekko::ABI fmt::fmt)
target_compile_features(moderngekko_legacy PUBLIC cxx_std_23)
target_include_directories(moderngekko_legacy PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy")

target_link_libraries(moderngekko PUBLIC ModernGekko::ABI PRIVATE fmt::fmt)
if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    target_sources(moderngekko PRIVATE
        src/runtime/dolphin_runtime.cpp
        vendor/dolphin/Source/Core/DolphinNoGUI/Platform.cpp
        vendor/dolphin/Source/Core/DolphinNoGUI/PlatformHeadless.cpp
    )
    if(WIN32)
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWin32.cpp)
        target_link_libraries(moderngekko PRIVATE dwmapi)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # The host app owns the window on iOS; this only reports the layer it
        # is given. See PlatformIOS.mm for why the ownership is inverted.
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformIOS.mm)
        set_source_files_properties(
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformIOS.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(moderngekko PRIVATE
            "-framework UIKit" "-framework QuartzCore" "-framework Metal"
            "${COREFOUNDATION_LIBRARY}")
        target_compile_definitions(moderngekko PRIVATE MODERNGEKKO_HAVE_UIKIT=1)
    elseif(APPLE)
        # Dolphin's windowed backend for macOS
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformMacos.mm)
        set_source_files_properties(
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformMacos.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(moderngekko PRIVATE
            "${APPKIT_LIBRARY}" "${COREFOUNDATION_LIBRARY}" "${IOK_LIBRARY}")
        target_compile_definitions(moderngekko PRIVATE MODERNGEKKO_HAVE_COCOA=1)
    endif()
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(moderngekko PRIVATE _ARCH_64=1)
    else()
        target_compile_definitions(moderngekko PRIVATE _ARCH_32=1)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
        target_compile_definitions(moderngekko PRIVATE _M_X86_64=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        target_compile_definitions(moderngekko PRIVATE _M_ARM_64=1)
    else()
        target_compile_definitions(moderngekko PRIVATE _M_GENERIC=1)
    endif()
    target_sources(moderngekko PRIVATE src/runtime/dolvm/dolvm_module.cpp)
    target_link_libraries(moderngekko PRIVATE core uicommon moderngekko_dolvm)
    if(TARGET PkgConfig::X11)
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformX11.cpp)
        target_link_libraries(moderngekko PRIVATE PkgConfig::X11)
        if(TARGET PkgConfig::XRANDR)
            target_link_libraries(moderngekko PRIVATE PkgConfig::XRANDR)
            target_compile_definitions(moderngekko PRIVATE HAVE_XRANDR=1)
        endif()
        target_compile_definitions(moderngekko PRIVATE HAVE_X11=1)
    endif()
    if(TARGET PkgConfig::WAYLAND)
        find_program(MODERNGEKKO_WAYLAND_SCANNER wayland-scanner
            HINTS "${_moderngekko_sdk_usr}/bin" REQUIRED)
        find_file(MODERNGEKKO_XDG_SHELL_XML xdg-shell.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/stable/xdg-shell"
                  "/usr/share/wayland-protocols/stable/xdg-shell"
            REQUIRED)
        find_file(MODERNGEKKO_XDG_DECORATION_XML xdg-decoration-unstable-v1.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/unstable/xdg-decoration"
                  "/usr/share/wayland-protocols/unstable/xdg-decoration"
            REQUIRED)
        find_file(MODERNGEKKO_IDLE_INHIBIT_XML idle-inhibit-unstable-v1.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/unstable/idle-inhibit"
                  "/usr/share/wayland-protocols/unstable/idle-inhibit"
            REQUIRED)
        find_library(MODERNGEKKO_WAYLAND_CURSOR_LIBRARY wayland-cursor
            HINTS "${_moderngekko_sdk_lib}" REQUIRED)
        find_library(MODERNGEKKO_XKBCOMMON_LIBRARY xkbcommon
            HINTS "${_moderngekko_sdk_lib}" REQUIRED)
        set(_moderngekko_wayland_generated
            "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
        set(_moderngekko_xdg_header
            "${_moderngekko_wayland_generated}/xdg-shell-client-protocol.h")
        set(_moderngekko_xdg_code
            "${_moderngekko_wayland_generated}/xdg-shell-protocol.c")
        set(_moderngekko_decoration_header
            "${_moderngekko_wayland_generated}/xdg-decoration-client-protocol.h")
        set(_moderngekko_decoration_code
            "${_moderngekko_wayland_generated}/xdg-decoration-protocol.c")
        set(_moderngekko_idle_inhibit_header
            "${_moderngekko_wayland_generated}/idle-inhibit-unstable-v1-client-protocol.h")
        set(_moderngekko_idle_inhibit_code
            "${_moderngekko_wayland_generated}/idle-inhibit-unstable-v1-protocol.c")
        add_custom_command(
            OUTPUT "${_moderngekko_xdg_header}" "${_moderngekko_xdg_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_XDG_SHELL_XML}" "${_moderngekko_xdg_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_XDG_SHELL_XML}" "${_moderngekko_xdg_code}"
            DEPENDS "${MODERNGEKKO_XDG_SHELL_XML}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${_moderngekko_decoration_header}" "${_moderngekko_decoration_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_XDG_DECORATION_XML}" "${_moderngekko_decoration_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_XDG_DECORATION_XML}" "${_moderngekko_decoration_code}"
            DEPENDS "${MODERNGEKKO_XDG_DECORATION_XML}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${_moderngekko_idle_inhibit_header}" "${_moderngekko_idle_inhibit_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_IDLE_INHIBIT_XML}" "${_moderngekko_idle_inhibit_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_IDLE_INHIBIT_XML}" "${_moderngekko_idle_inhibit_code}"
            DEPENDS "${MODERNGEKKO_IDLE_INHIBIT_XML}"
            VERBATIM)
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWayland.cpp
            "${_moderngekko_xdg_code}"
            "${_moderngekko_decoration_code}"
            "${_moderngekko_idle_inhibit_code}")
        target_include_directories(moderngekko PRIVATE
            "${_moderngekko_wayland_generated}")
        if(MODERNGEKKO_FRONTEND_SDK)
            target_link_libraries(moderngekko PRIVATE
                "${_moderngekko_sdk_lib}/libwayland-client.so")
        else()
            target_link_libraries(moderngekko PRIVATE PkgConfig::WAYLAND)
        endif()
        target_link_libraries(moderngekko PRIVATE
            "${MODERNGEKKO_WAYLAND_CURSOR_LIBRARY}"
            "${MODERNGEKKO_XKBCOMMON_LIBRARY}")
        target_compile_definitions(moderngekko PRIVATE HAVE_WAYLAND=1)
        target_compile_definitions(videovulkan PRIVATE HAVE_WAYLAND=1)
        if(MODERNGEKKO_FRONTEND_SDK)
            target_link_directories(moderngekko PRIVATE "${_moderngekko_sdk_lib}")
        endif()
    endif()
endif()
if(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
    target_sources(moderngekko_legacy PRIVATE vendor/dolphin_legacy/Common/DynamicLibrary.cpp)
    target_link_libraries(moderngekko_legacy PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(moderngekko_legacy PRIVATE MODERNGEKKO_ENABLE_DYNAMIC_MODULES=1)
    if(NOT MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        target_sources(moderngekko PRIVATE vendor/dolphin_legacy/Common/DynamicLibrary.cpp)
        target_link_libraries(moderngekko PRIVATE ${CMAKE_DL_LIBS})
    endif()
    target_compile_definitions(moderngekko PRIVATE MODERNGEKKO_ENABLE_DYNAMIC_MODULES=1)
endif()
target_compile_features(moderngekko PUBLIC cxx_std_23)
if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
else()
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy")
endif()

add_executable(moderngekko-module-info
    tools/module_info.cpp
)
target_link_libraries(moderngekko-module-info
    PRIVATE ModernGekko::Runtime
)

if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    if(TARGET SDL3-static)
        set(_moderngekko_sdl_target SDL3-static)
    elseif(TARGET SDL3::SDL3-shared)
        set(_moderngekko_sdl_target SDL3::SDL3-shared)
    elseif(TARGET SDL3::SDL3)
        set(_moderngekko_sdl_target SDL3::SDL3)
    else()
        message(FATAL_ERROR "No usable SDL3 target is available for the frontend")
    endif()
    add_executable(moderngekko-run
        tools/moderngekko_run.cpp
        tools/netplay_compatibility.cpp
        tools/netplay_session.cpp
        tools/frontend_config.cpp
        tools/dol_patch.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdl3.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(moderngekko-run PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Externals/imgui/imgui/backends")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(moderngekko-run PRIVATE _ARCH_64=1)
    else()
        target_compile_definitions(moderngekko-run PRIVATE _ARCH_32=1)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
        target_compile_definitions(moderngekko-run PRIVATE _M_X86_64=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        target_compile_definitions(moderngekko-run PRIVATE _M_ARM_64=1)
    else()
        target_compile_definitions(moderngekko-run PRIVATE _M_GENERIC=1)
    endif()
    if(MODERNGEKKO_FRONTEND_SDK AND UNIX AND NOT APPLE AND
       MODERNGEKKO_PORTABLE_DEFAULT_GAME)
        set_property(TARGET moderngekko-run PROPERTY
            BUILD_WITH_INSTALL_RPATH TRUE)
    elseif(MODERNGEKKO_FRONTEND_SDK)
        set_property(TARGET moderngekko-run PROPERTY BUILD_RPATH
            "${_moderngekko_sdk_lib}")
    endif()
    target_link_libraries(moderngekko-run PRIVATE
        ModernGekko::Runtime core uicommon imgui ${_moderngekko_sdl_target})
    set_target_properties(moderngekko-run PROPERTIES
        OUTPUT_NAME "${MODERNGEKKO_RUNNER_OUTPUT_NAME}")
    target_compile_definitions(moderngekko-run PRIVATE
        MODERNGEKKO_RUNNER_NAME="${MODERNGEKKO_RUNNER_OUTPUT_NAME}"
        MODERNGEKKO_USER_DIRECTORY_NAME="${MODERNGEKKO_USER_DIRECTORY_NAME}")
    if(MODERNGEKKO_DEFAULT_WINDOW_TITLE)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_DEFAULT_WINDOW_TITLE="${MODERNGEKKO_DEFAULT_WINDOW_TITLE}")
    endif()
    if(MODERNGEKKO_REQUIRED_DISC_ID)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_REQUIRED_DISC_ID="${MODERNGEKKO_REQUIRED_DISC_ID}")
    endif()
    if(MODERNGEKKO_REQUIRED_DOL_SHA256)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_REQUIRED_DOL_SHA256="${MODERNGEKKO_REQUIRED_DOL_SHA256}")
    endif()
    if(MODERNGEKKO_REQUIRED_REL_SHA256)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_REQUIRED_REL_SHA256="${MODERNGEKKO_REQUIRED_REL_SHA256}")
    endif()
    if(MODERNGEKKO_REQUIRED_ASSETS_SHA256)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_REQUIRED_ASSETS_SHA256="${MODERNGEKKO_REQUIRED_ASSETS_SHA256}")
    endif()
    if(MODERNGEKKO_PORTABLE_DEFAULT_GAME)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_PORTABLE_DEFAULT_GAME=1)
    endif()
    if(MODERNGEKKO_GAMECUBE_CONTROLLERS)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_GAMECUBE_CONTROLLERS=1)
    endif()
    if(MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS=1)
    endif()
    if(MODERNGEKKO_DOL_PATCH_MANIFEST)
        get_filename_component(_moderngekko_dol_patch_name
            "${MODERNGEKKO_DOL_PATCH_MANIFEST}" NAME)
        target_compile_definitions(moderngekko-run PRIVATE
            MODERNGEKKO_DOL_PATCH_MANIFEST="patches/${_moderngekko_dol_patch_name}")
    endif()
    add_custom_command(TARGET moderngekko-run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Data/Sys"
                "$<TARGET_FILE_DIR:moderngekko-run>/Sys")
    if(MODERNGEKKO_DOL_PATCH_MANIFEST)
        add_custom_command(TARGET moderngekko-run POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:moderngekko-run>/patches"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MODERNGEKKO_DOL_PATCH_MANIFEST}"
                    "$<TARGET_FILE_DIR:moderngekko-run>/patches/${_moderngekko_dol_patch_name}")
    endif()
    if(MODERNGEKKO_GAME_SETTINGS_OVERRIDE)
        get_filename_component(_moderngekko_settings_name
            "${MODERNGEKKO_GAME_SETTINGS_OVERRIDE}" NAME)
        add_custom_command(TARGET moderngekko-run POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MODERNGEKKO_GAME_SETTINGS_OVERRIDE}"
                    "$<TARGET_FILE_DIR:moderngekko-run>/Sys/GameSettings/${_moderngekko_settings_name}")
    endif()

    add_executable(moderngekko-launcher
        tools/moderngekko_launcher.cpp
        tools/frontend_config.cpp
        tools/dol_patch.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdl3.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(moderngekko-launcher PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Externals/imgui/imgui/backends")
    # The launcher only calls the game-inspection surface directly, but the
    # Dolphin static libraries still require the Host_* callbacks implemented
    # in the runtime object. Keep that object from being discarded by the
    # static linker.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        target_link_libraries(moderngekko-launcher PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>"
            discio core uicommon imgui ${_moderngekko_sdl_target})
    elseif((UNIX AND NOT APPLE) OR MINGW)
        target_link_libraries(moderngekko-launcher PRIVATE
            "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive"
            discio core uicommon imgui ${_moderngekko_sdl_target})
    else()
        target_link_libraries(moderngekko-launcher PRIVATE
            ModernGekko::Runtime discio imgui ${_moderngekko_sdl_target})
    endif()
    set_target_properties(moderngekko-launcher PROPERTIES
        OUTPUT_NAME "${MODERNGEKKO_LAUNCHER_OUTPUT_NAME}")
    target_compile_definitions(moderngekko-launcher PRIVATE
        MODERNGEKKO_FRONTEND_NAME="${MODERNGEKKO_FRONTEND_NAME}"
        MODERNGEKKO_RUNNER_FILENAME="${MODERNGEKKO_RUNNER_OUTPUT_NAME}"
        MODERNGEKKO_USER_DIRECTORY_NAME="${MODERNGEKKO_USER_DIRECTORY_NAME}"
        MODERNGEKKO_LOG_FILENAME="${MODERNGEKKO_LOG_FILENAME}")
    if(MODERNGEKKO_REQUIRED_DISC_ID)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_REQUIRED_DISC_ID="${MODERNGEKKO_REQUIRED_DISC_ID}")
    endif()
    if(MODERNGEKKO_REQUIRED_DOL_SHA256)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_REQUIRED_DOL_SHA256="${MODERNGEKKO_REQUIRED_DOL_SHA256}")
    endif()
    if(MODERNGEKKO_REQUIRED_REL_SHA256)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_REQUIRED_REL_SHA256="${MODERNGEKKO_REQUIRED_REL_SHA256}")
    endif()
    if(MODERNGEKKO_REQUIRED_ASSETS_SHA256)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_REQUIRED_ASSETS_SHA256="${MODERNGEKKO_REQUIRED_ASSETS_SHA256}")
    endif()
    if(MODERNGEKKO_PORTABLE_DEFAULT_GAME)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_PORTABLE_DEFAULT_GAME=1)
    endif()
    if(MODERNGEKKO_GAMECUBE_CONTROLLERS)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_GAMECUBE_CONTROLLERS=1)
    endif()
    if(MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS=1)
    endif()
    if(MODERNGEKKO_DOL_PATCH_MANIFEST)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_DOL_PATCH_MANIFEST="patches/${_moderngekko_dol_patch_name}")
    endif()
    if(MODERNGEKKO_DISC_PREPARER_OUTPUT_NAME)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_DISC_PREPARER_FILENAME="${MODERNGEKKO_DISC_PREPARER_OUTPUT_NAME}")
    endif()
    if(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID)
        target_compile_definitions(moderngekko-launcher PRIVATE
            MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID="${MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID}")
    endif()
    if(MODERNGEKKO_FRONTEND_SDK AND UNIX AND NOT APPLE AND
       MODERNGEKKO_PORTABLE_DEFAULT_GAME)
        set_property(TARGET moderngekko-launcher PROPERTY
            BUILD_WITH_INSTALL_RPATH TRUE)
    elseif(MODERNGEKKO_FRONTEND_SDK)
        set_property(TARGET moderngekko-launcher PROPERTY BUILD_RPATH
            "${_moderngekko_sdk_lib}")
    endif()
    if(UNIX AND NOT APPLE)
        set_target_properties(moderngekko-run moderngekko-launcher PROPERTIES
            INSTALL_RPATH "$ORIGIN/lib")
    endif()
    add_dependencies(moderngekko-launcher moderngekko-run)

    add_executable(moderngekko-port tools/moderngekko_port.cpp)
    target_link_libraries(moderngekko-port PRIVATE ModernGekko::Runtime)
    target_compile_definitions(moderngekko-port PRIVATE
        MODERNGEKKO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(DOLRECOMP_ENABLE_LLVM)
        target_compile_definitions(moderngekko-port PRIVATE
            MODERNGEKKO_DOLRECOMP_LLVM=1)
    endif()
    add_dependencies(moderngekko-port dolrecomp moderngekko-run)
    # A custom target rather than POST_BUILD on moderngekko-port. POST_BUILD only
    # fires when its own target relinks, so bumping DolRecomp rebuilt the
    # recompiler while leaving the stale copy in place -- and moderngekko-port
    # runs the copy beside itself, so it silently kept recompiling with the old
    # binary and reported success. It cannot hang off dolrecomp either:
    # add_custom_command(TARGET) requires the target be defined in the same
    # directory, and dolrecomp comes from add_subdirectory. A custom target is
    # always considered out of date, and copy_if_different makes that cheap.
    add_custom_target(moderngekko-port-recompiler ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:dolrecomp>"
                "$<TARGET_FILE_DIR:moderngekko-port>"
        DEPENDS dolrecomp
        COMMENT "Refreshing dolrecomp beside moderngekko-port")
    add_dependencies(moderngekko-port moderngekko-port-recompiler)
endif()

if(WIN32)
    set_target_properties(moderngekko PROPERTIES
        OUTPUT_NAME moderngekko_runtime)
    target_compile_definitions(moderngekko_legacy PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(moderngekko_dolphin_video PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    foreach(_moderngekko_windows_target
            moderngekko
            moderngekko-module-info
            moderngekko-run
            moderngekko-launcher
            moderngekko-port)
        if(TARGET ${_moderngekko_windows_target})
            target_compile_definitions(${_moderngekko_windows_target} PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX)
            if(MSVC)
                target_compile_options(${_moderngekko_windows_target} PRIVATE
                    /Zc:preprocessor)
            endif()
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(moderngekko_dol_patch_test
        tests/dol_patch_test.cpp
        tools/dol_patch.cpp)
    target_include_directories(moderngekko_dol_patch_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    target_compile_features(moderngekko_dol_patch_test PRIVATE cxx_std_23)
    add_test(NAME moderngekko.dol_patch COMMAND moderngekko_dol_patch_test)

    add_executable(moderngekko_frontend_config_test
        tests/frontend_config_test.cpp
        tools/frontend_config.cpp)
    target_include_directories(moderngekko_frontend_config_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    target_compile_features(moderngekko_frontend_config_test PRIVATE cxx_std_23)
    add_test(NAME moderngekko.frontend_config COMMAND moderngekko_frontend_config_test)

    # The paired-single fast path is only allowed to exist if it is
    # bit-identical to the exact code it falls back to, and only a differential
    # test can say that. It links the interpreter's float sources directly
    # rather than the whole runtime: what is under test is one file.
    add_executable(moderngekko_paired_single_test
        vendor/dolphin/GXRuntime/tests/paired_single_tests.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter_float.c
        vendor/dolphin/GXRuntime/src/core/cpu_interpreter_table.c
        vendor/dolphin/GXRuntime/src/core/cpu_exception.c
        vendor/dolphin/GXRuntime/src/core/cpu.c)
    target_include_directories(moderngekko_paired_single_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/src")
    add_test(NAME moderngekko.paired_single
             COMMAND moderngekko_paired_single_test)

    # Header-only, so the test needs no companion translation unit.
    add_executable(moderngekko_launcher_savestates_test
        tests/launcher_savestates_test.cpp)
    target_include_directories(moderngekko_launcher_savestates_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools"
        # For Core/SavestateLayout.h, which owns the listing rules. It depends on
        # nothing but the standard library, so this pulls in no Dolphin linkage.
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
    target_compile_features(moderngekko_launcher_savestates_test PRIVATE cxx_std_23)
    add_test(NAME moderngekko.launcher_savestates
             COMMAND moderngekko_launcher_savestates_test)

    add_executable(moderngekko_frontend_config_gamecube_test
        tests/frontend_config_test.cpp
        tools/frontend_config.cpp)
    target_include_directories(moderngekko_frontend_config_gamecube_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    target_compile_features(moderngekko_frontend_config_gamecube_test PRIVATE cxx_std_23)
    target_compile_definitions(moderngekko_frontend_config_gamecube_test PRIVATE
        MODERNGEKKO_GAMECUBE_CONTROLLERS=1)
    add_test(NAME moderngekko.frontend_config_gamecube
             COMMAND moderngekko_frontend_config_gamecube_test)

    if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        add_executable(moderngekko_netplay_protocol_test
            tests/netplay_protocol_test.cpp
            tools/netplay_compatibility.cpp)
        target_include_directories(moderngekko_netplay_protocol_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include")
        target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE
            MODERNGEKKO_NETPLAY_TEST_MOD_PATH="$<TARGET_FILE:moderngekko_test_mod>")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _ARCH_64=1)
        else()
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _ARCH_32=1)
        endif()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_X86_64=1)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_ARM_64=1)
        else()
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_GENERIC=1)
        endif()
        if(MINGW)
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive" core uicommon)
        elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>" core uicommon)
        elseif(UNIX AND NOT APPLE)
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive" core uicommon)
        else()
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                ModernGekko::Runtime core uicommon)
        endif()
        if(WIN32)
            # The same treatment the non-test Windows targets get from the loop
            # above, which runs before the test targets exist and so misses
            # them. WIN32_LEAN_AND_MEAN keeps windows.h from pulling in
            # winsock.h, which would clash with the winsock2.h this reaches
            # through Dolphin's networking headers ('sockaddr' redefinition).
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX)
            if(MSVC)
                # Dolphin's headers use __VA_OPT__, which the traditional
                # preprocessor cannot parse.
                target_compile_options(moderngekko_netplay_protocol_test PRIVATE
                    /Zc:preprocessor)
            endif()
        endif()
        add_test(NAME moderngekko.netplay_protocol COMMAND moderngekko_netplay_protocol_test)
    endif()

    add_executable(moderngekko_game_inspect_test tests/game_inspect_test.cpp)
    target_link_libraries(moderngekko_game_inspect_test PRIVATE ModernGekko::LegacyRuntime)
    add_test(NAME moderngekko.game_inspect COMMAND moderngekko_game_inspect_test)

    add_executable(moderngekko_module_abi_test
        tests/module_abi_test.c
    )
    target_link_libraries(moderngekko_module_abi_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.module_abi
             COMMAND moderngekko_module_abi_test)

    add_library(moderngekko_test_module SHARED
        tests/fixtures/test_module.c
    )
    target_link_libraries(moderngekko_test_module
        PRIVATE ModernGekko::ABI
    )
    set_target_properties(moderngekko_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "moderngekko_test_module"
    )
    add_library(moderngekko_missing_export_module SHARED
        tests/fixtures/missing_export.c)
    set_target_properties(moderngekko_missing_export_module PROPERTIES
        PREFIX "" OUTPUT_NAME "moderngekko_missing_export_module")

    add_executable(moderngekko_module_loader_test
        tests/module_loader_test.cpp
    )
    target_link_libraries(moderngekko_module_loader_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_compile_definitions(moderngekko_module_loader_test
        PRIVATE
            MODERNGEKKO_TEST_MODULE_PATH="$<TARGET_FILE:moderngekko_test_module>"
            MODERNGEKKO_MISSING_EXPORT_MODULE_PATH="$<TARGET_FILE:moderngekko_missing_export_module>"
    )
    add_dependencies(moderngekko_module_loader_test moderngekko_test_module
                     moderngekko_missing_export_module)
    add_test(NAME moderngekko.module_loader
             COMMAND moderngekko_module_loader_test)

    add_library(moderngekko_test_mod SHARED
        tests/fixtures/test_mod.c
    )
    target_link_libraries(moderngekko_test_mod
        PRIVATE ModernGekko::ABI
    )
    set_target_properties(moderngekko_test_mod PROPERTIES
        PREFIX ""
        OUTPUT_NAME "fixture.mgm"
    )
    add_executable(moderngekko_mod_loader_test
        tests/mod_loader_test.cpp
    )
    target_link_libraries(moderngekko_mod_loader_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_compile_definitions(moderngekko_mod_loader_test
        PRIVATE
            MODERNGEKKO_TEST_MOD_DIR="$<TARGET_FILE_DIR:moderngekko_test_mod>"
    )
    add_dependencies(moderngekko_mod_loader_test moderngekko_test_mod)
    if(TARGET moderngekko_netplay_protocol_test)
        add_dependencies(moderngekko_netplay_protocol_test moderngekko_test_mod)
    endif()
    add_test(NAME moderngekko.mod_loader
             COMMAND moderngekko_mod_loader_test)

    add_executable(moderngekko_cpu_primitives_test
        tests/cpu_primitives_test.cpp
    )
    target_link_libraries(moderngekko_cpu_primitives_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_include_directories(moderngekko_cpu_primitives_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
    )
    add_test(NAME moderngekko.cpu_primitives
             COMMAND moderngekko_cpu_primitives_test)

    if(WIN32)
        # Header-only, and Windows-only like the rule it covers.
        add_executable(moderngekko_cache_affinity_test
            tests/cache_affinity_test.cpp)
        target_include_directories(moderngekko_cache_affinity_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools")
        target_compile_features(moderngekko_cache_affinity_test PRIVATE cxx_std_23)
        add_test(NAME moderngekko.cache_affinity
                 COMMAND moderngekko_cache_affinity_test)
    endif()

    add_executable(moderngekko_address_space_test
        tests/address_space_test.cpp
    )
    target_link_libraries(moderngekko_address_space_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.address_space
             COMMAND moderngekko_address_space_test)

    add_library(moderngekko_runtime_test_module SHARED
        tests/fixtures/runtime_test_module.c
    )
    target_link_libraries(moderngekko_runtime_test_module
        PRIVATE ModernGekko::ABI
    )
    set_target_properties(moderngekko_runtime_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "moderngekko_runtime_test_module"
    )

    add_executable(moderngekko_runtime_test
        tests/runtime_test.cpp
    )
    target_link_libraries(moderngekko_runtime_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_compile_definitions(moderngekko_runtime_test
        PRIVATE
            MODERNGEKKO_RUNTIME_TEST_MODULE_PATH="$<TARGET_FILE:moderngekko_runtime_test_module>"
    )
    add_dependencies(moderngekko_runtime_test moderngekko_runtime_test_module)
    add_test(NAME moderngekko.runtime
             COMMAND moderngekko_runtime_test)

    add_executable(moderngekko_dol_loader_test
        tests/dol_loader_test.cpp
    )
    target_link_libraries(moderngekko_dol_loader_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.dol_loader
             COMMAND moderngekko_dol_loader_test)

    add_executable(moderngekko_boot_environment_test
        tests/boot_environment_test.cpp
    )
    target_link_libraries(moderngekko_boot_environment_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.boot_environment
             COMMAND moderngekko_boot_environment_test)

    add_executable(moderngekko_gekko_definitions_test
        tests/gekko_definitions_test.cpp
    )
    target_link_libraries(moderngekko_gekko_definitions_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_include_directories(moderngekko_gekko_definitions_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
    )
    add_test(NAME moderngekko.gekko_definitions
             COMMAND moderngekko_gekko_definitions_test)

    add_executable(moderngekko_interpreter_test
        tests/interpreter_test.cpp
    )
    target_link_libraries(moderngekko_interpreter_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.interpreter
             COMMAND moderngekko_interpreter_test)

    add_executable(moderngekko_hardware_core_test
        tests/hardware_core_test.cpp
    )
    target_link_libraries(moderngekko_hardware_core_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.hardware_core
             COMMAND moderngekko_hardware_core_test)

    add_executable(moderngekko_io_services_test
        tests/io_services_test.cpp
    )
    target_link_libraries(moderngekko_io_services_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.io_services
             COMMAND moderngekko_io_services_test)

    add_executable(moderngekko_audio_system_test
        tests/audio_system_test.cpp
    )
    target_link_libraries(moderngekko_audio_system_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.audio_system
             COMMAND moderngekko_audio_system_test)

    add_executable(moderngekko_gx_commands_test
        tests/gx_commands_test.cpp
    )
    target_link_libraries(moderngekko_gx_commands_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_commands
             COMMAND moderngekko_gx_commands_test)

    add_executable(moderngekko_gx_state_test
        tests/gx_state_test.cpp
    )
    target_link_libraries(moderngekko_gx_state_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_state
             COMMAND moderngekko_gx_state_test)

    add_executable(moderngekko_gx_vertex_test
        tests/gx_vertex_test.cpp
    )
    target_link_libraries(moderngekko_gx_vertex_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_vertex
             COMMAND moderngekko_gx_vertex_test)

    add_executable(moderngekko_gx_conformance_test
        tests/gx_conformance_test.cpp
    )
    target_link_libraries(moderngekko_gx_conformance_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_conformance
             COMMAND moderngekko_gx_conformance_test)

    add_executable(moderngekko_gx_texture_test
        tests/gx_texture_test.cpp
    )
    target_link_libraries(moderngekko_gx_texture_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_texture
             COMMAND moderngekko_gx_texture_test)

    add_executable(moderngekko_gx_pipeline_test
        tests/gx_pipeline_test.cpp
    )
    target_link_libraries(moderngekko_gx_pipeline_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_pipeline
             COMMAND moderngekko_gx_pipeline_test)

    add_executable(moderngekko_dolphin_shader_test
        tests/dolphin_shader_test.cpp
    )
    target_link_libraries(moderngekko_dolphin_shader_test
        PRIVATE ModernGekko::DolphinVideo
    )
    add_test(NAME moderngekko.dolphin_shader
             COMMAND moderngekko_dolphin_shader_test)
endif()
