#!/bin/bash
export TZ='Asia/Jakarta'
set -e

# Warna
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

BUILDDATE=$(date +%Y%m%d)
NAME="Swordx-Shisui"

# Install dependencies untuk Arch Linux (menggunakan paket yang tepat)
# sudo pacman -Syu --needed base-devel bc cpio nano ca-certificates curl flex bison git glibc openssl python openssh wget zip zstd clang aarch64-linux-gnu-gcc arm-none-eabi-gcc libarchive ccache

# Move & Extract Clang
if [ ! -d "clang" ]; then
    echo -e "${YELLOW}Clang not found, downloading...${NC}"
    wget https://github.com/kutemeikito/RastaMod69-Clang/releases/download/RastaMod69-Clang-20.0.0-release/RastaMod69-Clang-20.0.0.tar.gz
    mkdir clang 
    tar -xf RastaMod69-Clang-20.0.0.tar.gz -C clang 
    rm -rf RastaMod69-Clang-20.0.0.tar.gz
fi

# Set variable
export KBUILD_BUILD_USER="kiddie"
export KBUILD_BUILD_HOST="arch" # Diubah ke arch untuk menyesuaikan host

# Prepare
echo -e "${YELLOW}Preparing build config...${NC}"
make O=out -j$(nproc) ARCH=arm64 CC="$(pwd)/clang/bin/clang" CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1 surya_defconfig

# Execute
echo -e "${YELLOW}Executing build...${NC}"
if ! make O=out -j$(nproc) ARCH=arm64 CC="$(pwd)/clang/bin/clang" CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1 2>error.log; then
    cat error.log
    echo -e "${RED}Build failed, check error.log${NC}"
    exit 1
fi
grep -i "warning:" error.log > warning.log || true

# Package
echo -e "${YELLOW}Packaging kernel...${NC}"
if [ ! -d "AnyKernel3" ]; then
    echo "AnyKernel3 not found, cloning..."
    git clone --depth=1 https://github.com/ardia-kun/AnyKernel3 AnyKernel3
fi

echo -e "${YELLOW}Compressing Image with 7z (Ultra Gzip)...${NC}"
rm -f AnyKernel3/Image.gz
if command -v 7z >/dev/null 2>&1; then
    7z a -tgzip -mx=9 -mfb=258 -mpass=7 AnyKernel3/Image.gz out/arch/arm64/boot/Image > /dev/null
elif command -v pigz >/dev/null 2>&1; then
    pigz -11 -c out/arch/arm64/boot/Image > AnyKernel3/Image.gz
else
    gzip -9 -c out/arch/arm64/boot/Image > AnyKernel3/Image.gz
fi

# Zip it and upload it
cd AnyKernel3
ZIP_NAME="${NAME}+KSU-${BUILDDATE}.zip"
echo -e "${YELLOW}Creating ultra-compressed zip file...${NC}"
rm -f "$ZIP_NAME"
if command -v 7za >/dev/null 2>&1; then
    7za a -tzip -mx=9 -mfb=258 -mpass=7 "$ZIP_NAME" . -x!".git*" -x!"README.md" -x!"*.zip" > /dev/null
elif command -v 7z >/dev/null 2>&1; then
    7z a -tzip -mx=9 -mfb=258 -mpass=7 "$ZIP_NAME" . -x!".git*" -x!"README.md" -x!"*.zip" > /dev/null
else
    zip -r9 "$ZIP_NAME" . -x ".git*" -x "README.md" -x "*.zip"
fi

# Upload to Pixeldrain
echo -e "${YELLOW}Uploading to Pixeldrain...${NC}"
curl -T "$ZIP_NAME" -u :3aaaa5a9-2da7-4cbc-93f5-74bcf33b9e3f https://pixeldrain.com/api/file/

# Finish
cd ..
echo -e "${GREEN}Build finished successfully!${NC}"
