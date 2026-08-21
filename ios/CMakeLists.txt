# SPDX-License-Identifier: GPL-3.0-or-later
#
# The iOS app. Everything below the app target comes from ModernGekko, which
# brings Dolphin, DolRecomp and the DolVM interpreter with it.
#
# Configure with:
#   cmake -S ios -B build-ios -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DENABLE_GENERIC=ON
#
# ENABLE_GENERIC is not optional: it is what keeps every JIT backend out of the
# binary. See ios/README.md.

cmake_minimum_required(VERSION 3.20)
project(DolBundlerIOS C CXX OBJC OBJCXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "This project only builds for iOS. Pass -DCMAKE_SYSTEM_NAME=iOS.")
endif()

if(NOT ENABLE_GENERIC)
  # Guard rather than silently produce a binary that would be rejected: on
  # arm64 Dolphin would otherwise compile JitArm64 in, and an App Store build
  # may not generate executable code.
  message(FATAL_ERROR
    "ENABLE_GENERIC must be ON for iOS: it is what excludes the JIT backends. "
    "Re-run with -DENABLE_GENERIC=ON.")
endif()

set(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME ON CACHE BOOL "" FORCE)
set(MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ModernGekko
                 ${CMAKE_BINARY_DIR}/moderngekko EXCLUDE_FROM_ALL)

set(DOLRECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ModernGekko/vendor/dolphin/DolRecomp)
set(DOLPHIN_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ModernGekko/vendor/dolphin/Source/Core)

add_executable(DolBundler MACOSX_BUNDLE
  App/main.mm
  App/DBAppDelegate.mm
  App/DBLibrary.mm
  App/DBLibraryViewController.mm
  App/DBGameViewController.mm
  App/DBTouchPadView.mm
  bridge/dolbundler_core.c
  bridge/dolbundler_run.mm
)

target_include_directories(DolBundler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/App
  ${CMAKE_CURRENT_SOURCE_DIR}/bridge
  ${DOLRECOMP_DIR}/src
  ${DOLPHIN_CORE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../ModernGekko/include
)

target_link_libraries(DolBundler PRIVATE
  # The runtime, and the interpreter built against the chassis's CPUState.
  # Note this is moderngekko_dolvm and never DolRecomp's own dr_vm: the two
  # compile the same interpreter sources against different CPUState layouts.
  ModernGekko::Runtime
  moderngekko_dolvm
  # The compiler half, for importing a disc on device.
  dr_app
  # Dolphin's headers use fmt in inline code, so the app needs its include
  # path even though the app itself never formats anything. Linking the target
  # rather than hardcoding a path keeps this correct whether fmt comes from
  # Externals or elsewhere.
  fmt::fmt
)

target_compile_definitions(DolBundler PRIVATE MODERNGEKKO_HAVE_UIKIT=1)

target_link_libraries(DolBundler PRIVATE
  "-framework UIKit"
  "-framework Foundation"
  "-framework QuartzCore"
  "-framework Metal"
  "-framework MetalKit"
  "-framework AVFoundation"
  "-framework GameController"
  "-framework UniformTypeIdentifiers"
  "-framework AudioToolbox"
)

set_target_properties(DolBundler PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/App/Info.plist.in
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${DOLBUNDLER_BUNDLE_ID}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
  # The emulation thread is long-lived and the interpreter is the hot path;
  # letting Xcode strip or reorder it buys nothing and complicates profiling.
  XCODE_ATTRIBUTE_ENABLE_BITCODE NO
)

if(DOLBUNDLER_DEVELOPMENT_TEAM)
  set_target_properties(DolBundler PROPERTIES
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${DOLBUNDLER_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
endif()
