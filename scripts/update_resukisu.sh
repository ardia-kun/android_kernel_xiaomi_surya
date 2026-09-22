#!/usr/bin/env bash
# ==============================================================================
#  Auto-update ReSukiSU in-tree driver to latest upstream
#  Target: Linux 4.14 non-GKI with SuSFS v2.2.0 compatibility
# ==============================================================================

set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "=== Checking for ReSukiSU upstream updates ==="
TMP_KSU="$(mktemp -d /tmp/resukisu_upstream_XXXXXX)"
trap 'rm -rf "${TMP_KSU}"' EXIT

git clone --depth=50 -b main https://github.com/ReSukiSU/ReSukiSU.git "${TMP_KSU}"
pushd "${TMP_KSU}" > /dev/null
git fetch --unshallow 2>/dev/null || true
UPSTREAM_COMMIT=$(git rev-parse --short=8 HEAD)
UPSTREAM_COUNT=$(git rev-list --count HEAD)
UPSTREAM_TAG=$(git describe --abbrev=0 --tags 2>/dev/null || echo "v4.2.0-rc2")
UPSTREAM_CODE=$(( 30000 + UPSTREAM_COUNT + 700 ))
popd > /dev/null

echo "Latest upstream: ${UPSTREAM_TAG} (${UPSTREAM_COMMIT}), commits: ${UPSTREAM_COUNT}, version code: ${UPSTREAM_CODE}"

# Sync upstream kernel/ and uapi/ into drivers/kernelsu/
rsync -av --exclude=".git*" --exclude="Kbuild" --exclude="include/uapi" "${TMP_KSU}/kernel/" drivers/kernelsu/
rsync -av --exclude=".git*" "${TMP_KSU}/uapi/" drivers/kernelsu/include/uapi/

# Update Kbuild fallback version variables
sed -i "s/KSU_LOCAL_VERSION := [0-9]*/KSU_LOCAL_VERSION := ${UPSTREAM_COUNT}/g" drivers/kernelsu/Kbuild
sed -i "s/KSU_COMMIT_SHA  := [a-f0-9]*/KSU_COMMIT_SHA  := ${UPSTREAM_COMMIT}/g" drivers/kernelsu/Kbuild
sed -i "s/KSU_TAG_NAME    := .*/KSU_TAG_NAME    := ${UPSTREAM_TAG}/g" drivers/kernelsu/Kbuild

# Apply Linux 4.14 non-GKI & SuSFS compatibility patches
python3 - << 'PYEOF'
# 1. sucompat.h: Guard 5.18+ struct filename pointers with version check
with open('drivers/kernelsu/feature/sucompat.h', 'r') as f:
    content = f.read()
target_h = '// Handler functions exported for hook_manager\n#ifdef CONFIG_KSU_SUSFS'
replace_h = '// Handler functions exported for hook_manager\n#if defined(CONFIG_KSU_SUSFS) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))'
if target_h in content:
    content = content.replace(target_h, replace_h)
    with open('drivers/kernelsu/feature/sucompat.h', 'w') as f:
        f.write(content)

# 2. sucompat.c: Guard faccessat and stat handlers for 4.14 compatibility
with open('drivers/kernelsu/feature/sucompat.c', 'r') as f:
    content = f.read()
target_faccess = '#ifdef CONFIG_KSU_SUSFS\nint ksu_handle_faccessat(int *dfd, struct filename **filename'
replace_faccess = '#if defined(CONFIG_KSU_SUSFS) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))\nint ksu_handle_faccessat(int *dfd, struct filename **filename'
target_stat = '#ifdef CONFIG_KSU_SUSFS\nint ksu_handle_stat(int *dfd, struct filename **filename'
replace_stat = '#if defined(CONFIG_KSU_SUSFS) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))\nint ksu_handle_stat(int *dfd, struct filename **filename'
content = content.replace(target_faccess, replace_faccess).replace(target_stat, replace_stat)
with open('drivers/kernelsu/feature/sucompat.c', 'w') as f:
    f.write(content)

# 3. ksud_integration.c: Ensure SuSFS triggers non-kprobe implementation
with open('drivers/kernelsu/runtime/ksud_integration.c', 'r') as f:
    content = f.read()
target_manual = '\n#ifdef CONFIG_KSU_MANUAL_HOOK\n\n// NOTE:'
replace_manual = '\n#if defined(CONFIG_KSU_MANUAL_HOOK) || defined(CONFIG_KSU_SUSFS)\n\n// NOTE:'
if target_manual in content:
    content = content.replace(target_manual, replace_manual)
    with open('drivers/kernelsu/runtime/ksud_integration.c', 'w') as f:
        f.write(content)

# 4. klog.h: Demote pr_info to pr_debug to prevent dmesg log spam
with open('drivers/kernelsu/include/klog.h', 'r') as f:
    content = f.read()
if 'pr_debug' not in content:
    content += '\n#ifdef pr_info\n#undef pr_info\n#define pr_info(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)\n#endif\n'
    with open('drivers/kernelsu/include/klog.h', 'w') as f:
        f.write(content)
PYEOF

echo "✅ ReSukiSU updated successfully to ${UPSTREAM_TAG} (${UPSTREAM_COMMIT}) - code ${UPSTREAM_CODE}"

# Export environment variables for CI notification
if [ -n "${BASH_ENV}" ]; then
  echo "export RESUKISU_VERSION=\"${UPSTREAM_TAG} (${UPSTREAM_COMMIT}) [${UPSTREAM_CODE}]\"" >> "${BASH_ENV}"
fi
if [ -n "${GITHUB_ENV}" ]; then
  echo "RESUKISU_VERSION=${UPSTREAM_TAG} (${UPSTREAM_COMMIT}) [${UPSTREAM_CODE}]" >> "${GITHUB_ENV}"
fi
