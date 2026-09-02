#!/bin/sh
# Builds the menu bar app. No Xcode project — swiftc plus a hand-assembled
# bundle, because a menu bar item needs an .app and nothing more.
#
# The driver's absolute path is baked into Info.plist as GCDriverPath, so the
# app does not have to guess where the repo lives at runtime.
#
# Ad-hoc signing (`-s -`) is what stops Gatekeeper complaining on first launch.
# No entitlement is involved; a menu bar app needs none.
set -e

UI=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$UI/.." && pwd)
DRIVER="$ROOT/target/release/gc_controller"
APP="$UI/build/NSO GameCube Controller.app"

[ -x "$DRIVER" ] || cargo build --release --manifest-path "$ROOT/Cargo.toml"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

cat >"$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>NSO GameCube Controller</string>
	<key>CFBundleIdentifier</key><string>local.gc-controller.ui</string>
	<key>CFBundleExecutable</key><string>GcController</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>0.1.0</string>
	<key>LSMinimumSystemVersion</key><string>14.0</string>
	<!-- Menu bar app: no Dock icon, no app menu. -->
	<key>LSUIElement</key><true/>
	<key>GCDriverPath</key><string>$DRIVER</string>
</dict>
</plist>
PLIST

swiftc -O -parse-as-library \
	-o "$APP/Contents/MacOS/GcController" \
	"$UI/GcController.swift"

codesign --force -s - "$APP" >/dev/null 2>&1 || echo "note: ad-hoc signing failed; the app still runs"

echo "built: $APP"
echo "open it with:  open \"$APP\""
