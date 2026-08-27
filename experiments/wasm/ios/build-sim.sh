#!/usr/bin/env bash
# Build the WKWebView shell and run it in the iOS Simulator.
#
#   ./build-sim.sh                       build, install, launch, tail the log
#   URL=http://127.0.0.1:8712/?x=1 ./build-sim.sh
#
# The Simulator's 127.0.0.1 is the Mac's loopback, so `python3 ../serve.py game
# --iso ...` on the host needs no extra plumbing. No signing: the Simulator does
# not require it, which is the whole reason device work waits until the end.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/build/DolWeb.app"
BUNDLE_ID="com.bigmah.dolweb"
URL="${URL:-http://127.0.0.1:8712/}"
DEVICE="${DEVICE:-}"

command -v xcrun >/dev/null || { echo "Xcode command line tools not found" >&2; exit 1; }

SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
rm -rf "$APP"
mkdir -p "$APP"

xcrun clang -fobjc-arc -O2 \
  -target arm64-apple-ios17.0-simulator -isysroot "$SDK" \
  -framework UIKit -framework WebKit -framework Foundation \
  "$HERE/main.m" -o "$APP/DolWeb"

# The URL goes into an XML string, and a query string is full of `&` -- which is
# not a literal in XML. An unescaped one makes the plist unparseable, and the
# only symptom is `simctl install` saying "Missing bundle ID".
URL_XML="$(printf '%s' "$URL" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')"

cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleName</key><string>DolWeb</string>
  <key>CFBundleDisplayName</key><string>DolWeb</string>
  <key>CFBundleExecutable</key><string>DolWeb</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSRequiresIPhoneOS</key><true/>
  <key>MinimumOSVersion</key><string>17.0</string>
  <key>UILaunchScreen</key><dict/>
  <key>UIStatusBarHidden</key><true/>
  <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
  <key>UISupportedInterfaceOrientations</key>
  <array>
    <string>UIInterfaceOrientationLandscapeLeft</string>
    <string>UIInterfaceOrientationLandscapeRight</string>
    <string>UIInterfaceOrientationPortrait</string>
  </array>
  <!-- The dev server is plain HTTP on the host's loopback, which the Simulator
       shares. A shipping build would serve the page from the bundle. -->
  <key>NSAppTransportSecurity</key>
  <dict>
    <key>NSAllowsLocalNetworking</key><true/>
    <key>NSAllowsArbitraryLoads</key><true/>
  </dict>
  <key>DOLWEB_URL</key><string>$URL_XML</string>
</dict>
</plist>
PLIST

if [ -z "$DEVICE" ]; then
  DEVICE="$(xcrun simctl list devices available -j | python3 -c '
import json,sys
d=json.load(sys.stdin)["devices"]
best=None
for runtime, devs in d.items():
    if "iOS" not in runtime: continue
    for dev in devs:
        if not dev.get("isAvailable"): continue
        if dev["state"] == "Booted": print(dev["udid"]); raise SystemExit
        if best is None and "iPhone" in dev["name"]: best = dev["udid"]
print(best or "")')"
fi
[ -n "$DEVICE" ] || { echo "no available iOS Simulator device" >&2; exit 1; }

xcrun simctl bootstatus "$DEVICE" -b >/dev/null 2>&1 || xcrun simctl boot "$DEVICE" || true
xcrun simctl install "$DEVICE" "$APP"
xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || true
echo "launching $BUNDLE_ID on $DEVICE -> $URL"
xcrun simctl launch --console-pty --terminate-running-process \
  "$DEVICE" "$BUNDLE_ID" 2>&1 | sed -n '1,400p'
