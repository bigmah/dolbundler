#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/dolbundler-nearby.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -fblocks -I"$root/ios/bridge" \
  "$root/ios/bridge/DBNearbyTransport.mm" "$root/ios/tests/nearby_transport_test.mm" \
  -framework Foundation -framework Network -o "$test_dir/nearby-transport-test"
"$test_dir/nearby-transport-test"
