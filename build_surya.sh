#!/bin/bash
# Local build helper mirroring .circleci/config.yml for surya (SM6250/atoll)
set -eo pipefail
cd "$(dirname "$0")"

export ARCH=arm64
export SUBARCH=arm64
export PATH="$PWD/clang/bin:$PATH"
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache-surya}"
export KBUILD_BUILD_USER=kiddie
export KBUILD_BUILD_HOST=localhost
export TZ=Asia/Jakarta

MAKE_ARGS=(
  O=out -j"$(nproc)"
  ARCH=arm64
  CC="ccache clang"
  LD=ld.lld
  AR=llvm-ar
  NM=llvm-nm
  OBJCOPY=llvm-objcopy
  OBJDUMP=llvm-objdump
  STRIP=llvm-strip
  CROSS_COMPILE=aarch64-linux-gnu-
  CLANG_TRIPLE=aarch64-linux-gnu-
  LLVM_IAS=1
)

ccache -M 5G >/dev/null 2>&1 || true

if [ "$1" = "config" ]; then
  make "${MAKE_ARGS[@]}" surya_defconfig
  exit 0
fi

make "${MAKE_ARGS[@]}" surya_defconfig
make "${MAKE_ARGS[@]}" 2>&1 | tee build.log
