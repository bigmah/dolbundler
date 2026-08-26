#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
SOURCE="$ROOT/ModernGekko/vendor/dolphin/DolRecomp"
BUILD="${DOLRECOMP_LLVM20_BUILD:-$ROOT/build-dolrecomp-llvm20}"
LLVM_ROOT="${DOLRECOMP_LLVM20_ROOT:-/opt/homebrew/opt/llvm@20}"
LLVM_CONFIG="$LLVM_ROOT/bin/llvm-config"

if [ ! -x "$LLVM_CONFIG" ]; then
  echo "error: LLVM 20 is required at $LLVM_ROOT (install Homebrew llvm@20)" >&2
  exit 1
fi

LLVM_VERSION=$($LLVM_CONFIG --version)
case "$LLVM_VERSION" in
  20.*) ;;
  *)
    echo "error: pinned toolchain must be LLVM 20, found $LLVM_VERSION" >&2
    exit 1
    ;;
esac

case " $($LLVM_CONFIG --targets-built) " in
  *" AArch64 "*) ;;
  *)
    echo "error: LLVM $LLVM_VERSION was built without AArch64" >&2
    exit 1
    ;;
esac

echo "DolRecomp source: $SOURCE"
echo "DolRecomp revision: $(git -C "$ROOT" rev-parse HEAD)"
echo "LLVM: $LLVM_VERSION ($LLVM_ROOT)"
echo "LLVM targets: $($LLVM_CONFIG --targets-built)"
echo "Host build: $BUILD"

cmake -S "$SOURCE" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOLRECOMP_ENABLE_LLVM=ON \
  -DLLVM_DIR="$LLVM_ROOT/lib/cmake/llvm"
cmake --build "$BUILD" --target dolrecomp test_llvm_backend test_llvm_execute \
  test_llvm_pipeline

echo "host dolrecomp: $BUILD/dolrecomp"
