#!/bin/bash
set -euo pipefail
if [[ $# != 1 ]]; then
  echo "Usage: $0 <booted iOS simulator UUID>" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/dolbundler-nearby-lifecycle.XXXXXX")"
bundle_id="com.bigmah.dolbundler.nearby-lifecycle-tests"
trap 'xcrun simctl uninstall "$1" "$bundle_id" >/dev/null 2>&1 || true; rm -rf "$test_dir"' EXIT
app="$test_dir/NearbyLifecycleTests.app"
mkdir "$app"
sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
xcrun --sdk iphonesimulator clang++ -std=c++20 -fobjc-arc -fblocks \
  -target "$(uname -m)-apple-ios18.0-simulator" -isysroot "$sdk" \
  -I"$root/ios/App" -I"$root/ios/bridge" \
  "$root/ios/App/DBNearbyViewController.mm" "$root/ios/tests/nearby_lifecycle_test.mm" \
  -framework UIKit -framework Foundation -framework Network -o "$app/NearbyLifecycleTests"
cat > "$app/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.bigmah.dolbundler.nearby-lifecycle-tests</string>
<key>CFBundleExecutable</key><string>NearbyLifecycleTests</string>
<key>CFBundleName</key><string>NearbyLifecycleTests</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>MinimumOSVersion</key><string>18.0</string>
<key>LSRequiresIPhoneOS</key><true/>
<key>UILaunchScreen</key><dict/>
</dict></plist>
PLIST
xcrun simctl install "$1" "$app"
# --console waits for test completion; the explicit result also catches a
# simulator launch that exits without executing the tests.
xcrun simctl launch --console "$1" "$bundle_id" 2>&1 | tee "$test_dir/result.log"
rg -q 'Nearby lifecycle tests: PASSED \(0 failures\)' "$test_dir/result.log"
