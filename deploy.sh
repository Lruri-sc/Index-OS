#!/bin/sh
# Index -> UTM deploy. Swaps the kernel (always) and the rootfs (only when the
# local raw image is newer than the deployed qcow2) in the UTM bundle, with a
# timestamped backup of each. Refuses while the VM is running (files in use).
#
# Used for "auto-deploy after build+verify": run this once a new keeper is built
# and verified. Rollback: with the VM stopped, copy the matching .bak.<ts> back.
set -e
UTM="/Users/lruri/Library/Containers/com.utmapp.UTM/Data/Documents/Index.utm/Data"
HERE="/Users/lruri/Desktop/Index/arm-Index"

if pgrep -x qemu-system-aarch64 >/dev/null 2>&1 || pgrep -x QEMUHelper >/dev/null 2>&1; then
  echo "deploy: Index VM is running -- stop it in UTM, then re-run."
  exit 2
fi

TS=$(date +%H%M%S)

# Kernel (always).
cp "$UTM/Index-Image" "$UTM/Index-Image.bak.$TS"
cp "$HERE/build/Image" "$UTM/Index-Image"
echo "deploy: kernel -> $(shasum "$UTM/Index-Image" | cut -c1-8)  (backup Index-Image.bak.$TS)"

# Rootfs (only when the local raw image changed since the last deploy).
if [ -f "$HERE/build/ext2-jvm-26.img" ] && [ "$HERE/build/ext2-jvm-26.img" -nt "$UTM/ext2.qcow2" ]; then
  cp "$UTM/ext2.qcow2" "$UTM/ext2.qcow2.bak.$TS"
  qemu-img convert -f raw -O qcow2 "$HERE/build/ext2-jvm-26.img" "$UTM/ext2.qcow2.new"
  mv "$UTM/ext2.qcow2.new" "$UTM/ext2.qcow2"
  echo "deploy: rootfs -> updated  (backup ext2.qcow2.bak.$TS)"
else
  echo "deploy: rootfs unchanged -- kept"
fi
echo "deploy: done TS=$TS"
