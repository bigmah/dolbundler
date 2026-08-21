#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build DolBundler.app for iOS, and optionally install it to a connected device.
#
#   ./ios/build.sh                 configure and build
#   ./ios/build.sh --install       build, then install to the connected device
#   ./ios/build.sh --clean         throw away the build directory first
#
# Signing: set DOLBUNDLER_TEAM to your Apple Developer team ID. `--install`
# needs one; a plain build does not.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$ROOT/build-ios"

BUNDLE_ID="${DOLBUNDLER_BUNDLE_ID:-com.bigmah.dolbundler}"
TEAM="${DOLBUNDLER_TEAM:-}"

INSTALL=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --install) INSTALL=1 ;;
    --clean) CLEAN=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

bold=$'\033[1m'; off=$'\033[0m'
step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$off"; }

for tool in cmake xcodebuild; do
  command -v "$tool" >/dev/null || { echo "$tool is required but not installed" >&2; exit 1; }
done

# Dolphin's third-party externals are still submodules; the rest of the tree is
# in this repo. A plain clone leaves them empty.
if [ ! -f "$ROOT/ModernGekko/vendor/dolphin/Externals/fmt/fmt/CMakeLists.txt" ]; then
  step "Fetching Dolphin's externals"
  git -C "$ROOT" submodule update --init --recursive --depth 1
fi

[ "$CLEAN" -eq 1 ] && rm -rf "$BUILD"

step "Configuring"
# ENABLE_GENERIC is what keeps every JIT backend out of the binary. The
# CMakeLists refuses to configure without it; see README.md for why.
cmake -S "$HERE" -B "$BUILD" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_GENERIC=ON \
  -DDOLBUNDLER_BUNDLE_ID="$BUNDLE_ID" \
  ${TEAM:+-DDOLBUNDLER_DEVELOPMENT_TEAM="$TEAM"}

step "Building"
if [ -n "$TEAM" ]; then
  xcodebuild -project "$BUILD/DolBundlerIOS.xcodeproj" -target DolBundler \
    -configuration Release -sdk iphoneos -arch arm64 \
    DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic \
    -allowProvisioningUpdates build
else
  echo "    no DOLBUNDLER_TEAM set: building unsigned"
  xcodebuild -project "$BUILD/DolBundlerIOS.xcodeproj" -target DolBundler \
    -configuration Release -sdk iphoneos -arch arm64 \
    CODE_SIGNING_ALLOWED=NO build
fi

APP="$BUILD/Release-iphoneos/DolBundler.app"
[ -d "$APP" ] || { echo "build did not produce $APP" >&2; exit 1; }
step "Built $APP"
du -sh "$APP"

if [ "$INSTALL" -eq 1 ]; then
  [ -n "$TEAM" ] || { echo "--install needs DOLBUNDLER_TEAM set" >&2; exit 1; }
  step "Installing to the device"

  # devicectl replaced ios-deploy in Xcode 15 and ships with Xcode. Its plain
  # listing does not say whether a device is actually reachable -- a phone can
  # be "paired" and still have no open tunnel -- so the JSON is parsed instead.
  devices_json="$(mktemp -t dolbundler-devices)"
  xcrun devicectl list devices --json-output "$devices_json" >/dev/null 2>&1

  device="$(python3 - "$devices_json" <<'PY'
import json, sys
devices = json.load(open(sys.argv[1]))["result"]["devices"]
# Prefer one with a live tunnel; fall back to any paired device so the install
# is at least attempted and devicectl can report why it failed.
for want_tunnel in (True, False):
    for d in devices:
        conn = d.get("connectionProperties", {})
        if (conn.get("tunnelState") == "connected") == want_tunnel:
            print(d["identifier"])
            sys.exit(0)
PY
)"
  rm -f "$devices_json"

  if [ -z "$device" ]; then
    echo "no paired device found." >&2
    echo "Plug the iPhone in, unlock it, and tap Trust." >&2
    exit 1
  fi

  if ! xcrun devicectl device install app --device "$device" "$APP"; then
    echo >&2
    echo "Install failed. The usual cause is that the device is paired but not" >&2
    echo "reachable right now -- devicectl needs an open tunnel, not just a" >&2
    echo "pairing record. Check that the iPhone is:" >&2
    echo "  * plugged in over USB, or on the same Wi-Fi as this Mac" >&2
    echo "  * unlocked" >&2
    echo "  * showing Developer Mode on under Settings > Privacy & Security" >&2
    echo >&2
    echo "Current state:" >&2
    xcrun devicectl list devices 2>/dev/null | tail -n +2 >&2
    exit 1
  fi

  echo "    installed. On first launch, trust the developer certificate under"
  echo "    Settings > General > VPN & Device Management."
fi
