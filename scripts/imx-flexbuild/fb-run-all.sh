#!/bin/bash
# fb-run-all.sh -- rebuild all three targets with the ML stack disabled.
#
# Previous run: all three aborted at `tensorflow-lite` during the apps stage,
# which runs BEFORE flash.bin/boot/merge-apps/packrfs -- so no bootable image
# was produced, while a driver bug reported every one of them as "ok".
#
# rootfs + kernel are already built in both trees, so those stages should be
# skipped and this should pick up near where it failed.

set -uo pipefail

echo "############ STOCK (baseline) ############"
DISABLE_ML=1 /data/fb-build.sh imx93frdm imx95-15x15-frdm
stock=$?

echo
echo "############ PREEMPT_RT ############"
DISABLE_ML=1 FBDIR=/data/flexbuild-rt EXTRA_KCONFIG=rt.config \
    VERIFY_KCONFIG=CONFIG_PREEMPT_RT /data/fb-build.sh imx93frdm
rt=$?

echo
echo "################ FINAL $(date -Is) ################"
echo "stock rc=$stock   rt rc=$rt"
for f in /data/flexbuild/logs/*.status /data/flexbuild-rt/logs/*.status; do
    [ -f "$f" ] && printf '  %-52s %s\n' "$f" "$(cat "$f")"
done
echo "--- artifacts ---"
ls -lh /data/flexbuild/build_*/images/ /data/flexbuild-rt/build_*/images/ 2>/dev/null
find /data/flexbuild /data/flexbuild-rt -maxdepth 3 -name "*.tar.zst" -o -maxdepth 3 -name "*flash.bin" 2>/dev/null
