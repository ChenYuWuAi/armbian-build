#!/bin/bash
# Build a 6.12.58 kernel matching the running Armbian (6.12.58-current-sm8250)
# with Armbian's sm8250-6.12 patch series + our audio patches (0050-0052).
#
# Uses the DEVICE's own kernel config (build/device.config) so the result
# matches what is already installed, then adds:
#   0050 ADM COPP master gain volume      (q6adm + ALSA controls)
#   0051 ASM stream volume                (q6asm, the upstream-missing gain)
#   0052 COPP topology override           (q6routing, runtime tunable)
#
# Non-destructive: the pristine tree can be re-extracted from
# build/linux-6.12.58-gh.tar.gz.  Failing patches are reported, not fatal.
set -u
TREE=/home/axis/axis_rnd/work/kernel/build/linux-6.12.58
AB=/home/axis/axis_rnd/work/kernel/build/armbian-build
PDIR="$AB/patch/kernel/archive/sm8250-6.12"
CFG=/home/axis/axis_rnd/work/kernel/build/device.config
CROSS=aarch64-linux-gnu-
export ARCH=arm64 CROSS_COMPILE=$CROSS

cd "$TREE" || exit 1
echo "### disk:"; df -h . | tail -1

echo "### applying Armbian sm8250-6.12 patches (in filename order)"
applied=0; skipped=0
for p in "$PDIR"/[0-9]*.patch; do
  n=$(basename "$p")
  if patch -p1 --forward --batch --dry-run < "$p" >/dev/null 2>&1; then
    if patch -p1 --forward --batch < "$p" >/dev/null 2>&1; then
      echo "OK    $n"; applied=$((applied+1))
    else
      echo "FAIL  $n"; skipped=$((skipped+1))
    fi
  else
    echo "SKIP  $n  (does not apply cleanly)"; skipped=$((skipped+1))
  fi
done
echo "### patches applied=$applied skipped=$skipped"

echo "### config from device"
cp "$CFG" .config || exit 1
make ARCH=$ARCH CROSS_COMPILE=$CROSS olddefconfig 2>&1 | tail -5

echo "### build"
make -j"$(nproc)" ARCH=$ARCH CROSS_COMPILE=$CROSS Image modules dtbs
rc=$?
echo "### BUILD_EXIT=$rc"
ls -la arch/arm64/boot/Image 2>/dev/null
ls -la arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-*.dtb 2>/dev/null
echo "### done"
