#!/usr/bin/env bash
# ==============================================================================
#  SWORDX-SHISUI KERNEL BUILD SCRIPT
#  Target: Xiaomi POCO X3 NFC / POCO X3 (surya / karna - SM7150)
#  Author: ardia-kun (kiddie@arch)
# ==============================================================================

set -eo pipefail
export TZ="Asia/Jakarta"

# ------------------------------------------------------------------------------
# Colors & Styles (ANSI TrueColor / 256-color)
# ------------------------------------------------------------------------------
BOLD="\033[1m"
DIM="\033[2m"
ITALIC="\033[3m"
UNDERLINE="\033[4m"

RED="\033[38;5;196m"
GREEN="\033[38;5;46m"
YELLOW="\033[38;5;226m"
BLUE="\033[38;5;39m"
PURPLE="\033[38;5;135m"
CYAN="\033[38;5;51m"
WHITE="\033[38;5;231m"
GRAY="\033[38;5;245m"
DARK_GRAY="\033[38;5;238m"
NC="\033[0m" # No Color

# ------------------------------------------------------------------------------
# Build Variables
# ------------------------------------------------------------------------------
START_TIME=$(date +%s)
BUILD_DATE_NUM=$(date +%Y%m%d)
BUILD_DATE_READABLE=$(date +"%d %b %Y, %H:%M:%S %Z")
KERNEL_NAME="Swordx-Shisui"
DEVICE_CODENAME="surya"
DEVICE_NAME="Xiaomi POCO X3 NFC / POCO X3"
DEFCONFIG="surya_defconfig"
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")
COMMIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
JOBS=$(nproc --all)

export KBUILD_BUILD_USER="kiddie"
export KBUILD_BUILD_HOST="arch"

# ------------------------------------------------------------------------------
# Helper Functions
# ------------------------------------------------------------------------------
print_banner() {
    clear 2>/dev/null || true
    echo -e "${CYAN}${BOLD}"
    echo "  ╔═══════════════════════════════════════════════════════════════════════╗"
    echo "  ║                                                                       ║"
    echo "  ║      ⚔️   ${WHITE}SWORDX-SHISUI KERNEL BUILD SYSTEM${CYAN}   ⚔️                     ║"
    echo "  ║          ${YELLOW}Optimized for Qualcomm Snapdragon 732G (SM7150)${CYAN}             ║"
    echo "  ║                                                                       ║"
    echo "  ╚═══════════════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_info_card() {
    echo -e "${BLUE}${BOLD}┌─[ 📋 BUILD CONFIGURATION ]${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Kernel Name   :${NC} ${GREEN}${KERNEL_NAME} + KSU / SUSFS${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Target Device :${NC} ${CYAN}${DEVICE_NAME} (${DEVICE_CODENAME})${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Branch/Commit :${NC} ${PURPLE}${BRANCH}${NC} (${YELLOW}${COMMIT_HASH}${NC})"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Builder Host  :${NC} ${WHITE}${KBUILD_BUILD_USER}@${KBUILD_BUILD_HOST}${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Defconfig     :${NC} ${YELLOW}${DEFCONFIG}${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}CPU Threads   :${NC} ${GREEN}${JOBS} Cores${NC}"
    echo -e "${BLUE}${BOLD}│${NC}  ${WHITE}${BOLD}Build Time    :${NC} ${WHITE}${BUILD_DATE_READABLE}${NC}"
    echo -e "${BLUE}${BOLD}└────────────────────────────────────────────────────────────────────────${NC}"
    echo ""
}

print_step() {
    local step_num="$1"
    local total_steps="$2"
    local icon="$3"
    local message="$4"
    echo -e "${CYAN}${BOLD}[${step_num}/${total_steps}]${NC} ${icon}  ${WHITE}${BOLD}${message}${NC}"
}

format_time() {
    local seconds=$1
    local minutes=$((seconds / 60))
    local rem_seconds=$((seconds % 60))
    if [ $minutes -gt 0 ]; then
        echo "${minutes}m ${rem_seconds}s"
    else
        echo "${rem_seconds}s"
    fi
}

human_size() {
    local file="$1"
    if [ -f "$file" ]; then
        du -h "$file" | awk "{print \$1}"
    else
        echo "0B"
    fi
}

# ------------------------------------------------------------------------------
# 1. Toolchain & Clang Setup
# ------------------------------------------------------------------------------
print_banner
print_info_card

print_step "1" "6" "🔍" "Checking Toolchain & Compiler..."

if [ ! -d "clang" ]; then
    echo -e "      ${YELLOW}⚡ Clang not found locally. Downloading RastaMod69 Clang 20.0.0...${NC}"
    wget -q --show-progress "https://github.com/kutemeikito/RastaMod69-Clang/releases/download/RastaMod69-Clang-20.0.0-release/RastaMod69-Clang-20.0.0.tar.gz"
    mkdir -p clang
    tar -xf RastaMod69-Clang-20.0.0.tar.gz -C clang
    rm -f RastaMod69-Clang-20.0.0.tar.gz
    echo -e "      ${GREEN}✔ Clang installed successfully!${NC}"
fi

CLANG_BIN="$(pwd)/clang/bin/clang"
if [ ! -f "$CLANG_BIN" ]; then
    echo -e "      ${RED}❌ Error: Clang binary not found at $CLANG_BIN!${NC}"
    exit 1
fi

CLANG_VER=$("$CLANG_BIN" --version | head -n 1)
echo -e "      ${GRAY}Compiler:${NC} ${GREEN}${CLANG_VER}${NC}"
echo ""

export PATH="$(pwd)/clang/bin:$PATH"

# ------------------------------------------------------------------------------
# 2. Prepare Kernel Configuration
# ------------------------------------------------------------------------------
print_step "2" "6" "⚙️" "Generating Defconfig (${DEFCONFIG})..."

make O=out -j${JOBS} ARCH=arm64 CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1 ${DEFCONFIG} > /dev/null
echo -e "      ${GREEN}✔ Configuration created successfully in out/.config${NC}"
echo ""

# ------------------------------------------------------------------------------
# 3. Kernel Compilation
# ------------------------------------------------------------------------------
print_step "3" "6" "🛠️" "Compiling Kernel with ThinLTO (Image & DTBO)..."
BUILD_START=$(date +%s)

if ! make O=out -j${JOBS} ARCH=arm64 CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1 2>error.log; then
    echo ""
    echo -e "${RED}${BOLD}╔═══════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}${BOLD}║                      ❌ BUILD FAILED WITH ERRORS                      ║${NC}"
    echo -e "${RED}${BOLD}╚═══════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    tail -n 25 error.log
    echo ""
    echo -e "${RED}Please inspect ${YELLOW}error.log${RED} for detailed diagnostics.${NC}"
    exit 1
fi

BUILD_END=$(date +%s)
COMPILE_DURATION=$((BUILD_END - BUILD_START))
grep -i "warning:" error.log > warning.log 2>/dev/null || true
WARNING_COUNT=$(wc -l < warning.log 2>/dev/null || echo "0")

IMAGE_PATH="out/arch/arm64/boot/Image"
if [ ! -f "$IMAGE_PATH" ]; then
    echo -e "      ${RED}❌ Error: Kernel Image not found at $IMAGE_PATH!${NC}"
    exit 1
fi

IMAGE_RAW_SIZE=$(human_size "$IMAGE_PATH")
echo -e "      ${GREEN}✔ Kernel compilation succeeded in $(format_time $COMPILE_DURATION)!${NC}"
echo -e "      ${GRAY}Raw Image Size:${NC} ${YELLOW}${IMAGE_RAW_SIZE}${NC} | ${GRAY}Warnings:${NC} ${YELLOW}${WARNING_COUNT}${NC}"
echo ""

# ------------------------------------------------------------------------------
# 4. Compression (Image.gz)
# ------------------------------------------------------------------------------
print_step "4" "6" "🗜️" "Compressing Kernel Image with Ultra Gzip..."

if [ ! -d "AnyKernel3" ]; then
    echo -e "      ${YELLOW}Cloning AnyKernel3 template...${NC}"
    git clone --depth=1 https://github.com/ardia-kun/AnyKernel3 AnyKernel3 >/dev/null 2>&1
fi

rm -f AnyKernel3/Image.gz

if command -v 7z >/dev/null 2>&1; then
    7z a -tgzip -mx=9 -mfb=258 -mpass=7 AnyKernel3/Image.gz "$IMAGE_PATH" > /dev/null
elif command -v pigz >/dev/null 2>&1; then
    pigz -11 -c "$IMAGE_PATH" > AnyKernel3/Image.gz
else
    gzip -9 -c "$IMAGE_PATH" > AnyKernel3/Image.gz
fi

GZ_SIZE=$(human_size "AnyKernel3/Image.gz")
echo -e "      ${GREEN}✔ Compressed Image.gz:${NC} ${YELLOW}${GZ_SIZE}${NC}"
echo ""

# ------------------------------------------------------------------------------
# 5. Packaging AnyKernel3 Flashable ZIP
# ------------------------------------------------------------------------------
print_step "5" "6" "📦" "Packaging Flashable Zip (AnyKernel3)..."

cd AnyKernel3
ZIP_NAME="${KERNEL_NAME}+KSU-${BUILD_DATE_NUM}.zip"
rm -f "$ZIP_NAME"

if command -v 7za >/dev/null 2>&1; then
    7za a -tzip -mx=9 -mfb=258 -mpass=7 "$ZIP_NAME" . -x!".git*" -x!"README.md" -x!"*.zip" > /dev/null
elif command -v 7z >/dev/null 2>&1; then
    7z a -tzip -mx=9 -mfb=258 -mpass=7 "$ZIP_NAME" . -x!".git*" -x!"README.md" -x!"*.zip" > /dev/null
else
    zip -r9 "$ZIP_NAME" . -x ".git*" -x "README.md" -x "*.zip" > /dev/null
fi

ZIP_SIZE=$(human_size "$ZIP_NAME")
SHA256_HASH=$(sha256sum "$ZIP_NAME" | awk "{print \$1}")
MD5_HASH=$(md5sum "$ZIP_NAME" | awk "{print \$1}")

echo -e "      ${GREEN}✔ Package created:${NC} ${PURPLE}${BOLD}${ZIP_NAME}${NC} (${YELLOW}${ZIP_SIZE}${NC})"
echo -e "      ${GRAY}SHA256:${NC} ${WHITE}${SHA256_HASH:0:32}...${NC}"
echo ""

# ------------------------------------------------------------------------------
# 6. Uploading to Pixeldrain
# ------------------------------------------------------------------------------
print_step "6" "6" "🌐" "Uploading to Pixeldrain Cloud..."

UPLOAD_RESP=$(curl -s -T "$ZIP_NAME" -u :3aaaa5a9-2da7-4cbc-93f5-74bcf33b9e3f https://pixeldrain.com/api/file/ || true)
FILE_ID=$(echo "$UPLOAD_RESP" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

if [ -n "$FILE_ID" ]; then
    DOWNLOAD_URL="https://pixeldrain.com/u/${FILE_ID}"
    echo -e "      ${GREEN}✔ Upload completed successfully!${NC}"
else
    DOWNLOAD_URL="Upload failed or offline"
    echo -e "      ${YELLOW}⚠️ Notice: Upload response: ${UPLOAD_RESP}${NC}"
fi

cd ..
TOTAL_TIME=$(( $(date +%s) - START_TIME ))

# ------------------------------------------------------------------------------
# Summary Card
# ------------------------------------------------------------------------------
echo ""
echo -e "${GREEN}${BOLD}╭───────────────────────────────────────────────────────────────────────╮${NC}"
echo -e "${GREEN}${BOLD}│                 🎉  BUILD FINISHED SUCCESSFULLY!  🎉                   │${NC}"
echo -e "${GREEN}${BOLD}├───────────────────────────────────────────────────────────────────────┤${NC}"
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}Output Package :${NC} ${PURPLE}${BOLD}${ZIP_NAME}${NC}"
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}Package Size   :${NC} ${YELLOW}${ZIP_SIZE}${NC}"
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}MD5 Checksum   :${NC} ${WHITE}${MD5_HASH}${NC}"
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}Compilation    :${NC} ${CYAN}$(format_time $COMPILE_DURATION)${NC}"
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}Total Time     :${NC} ${CYAN}$(format_time $TOTAL_TIME)${NC}"
if [ -n "$FILE_ID" ]; then
echo -e "${GREEN}${BOLD}│${NC}  ${WHITE}${BOLD}Download Link  :${NC} ${BLUE}${UNDERLINE}${DOWNLOAD_URL}${NC}"
fi
echo -e "${GREEN}${BOLD}╰───────────────────────────────────────────────────────────────────────╯${NC}"
echo ""
