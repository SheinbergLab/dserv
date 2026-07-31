#!/bin/bash
# fb-mkwic.sh -- build all-in-one SD images (.wic.zst) for every built target.
#
# flex-installer's mkwic assembles composite firmware + boot partition + ext4
# rootfs into one image via a loop device, then zstd-compresses it.  Loop
# devices and mkfs.ext4 need Linux + privilege, so this runs in the same
# privileged container the build used.  The result is a single file per board
# that macOS can write with plain dd -- no partitioning needed on the Mac.
#
# Output: /data/imx-sdcards/sdcard_<machine>[-rt].wic.zst  (+ .sha256)
#
# mkwic drops its output in the tree root as rootfs_<distro>_<machine>.wic.zst,
# which is a misleading name for a full-disk image -- renamed on collection.

set -uo pipefail

IMAGE=fbdebian:13-flexbuild
OUT=/data/imx-sdcards
BOOT=boot_IMX_arm64_lts_6.12.49.tar.zst

mkdir -p "$OUT"

# tree | machine | suffix
TARGETS=(
    "/data/flexbuild|imx93frdm|"
    "/data/flexbuild|imx95-15x15-frdm|"
    "/data/flexbuild-rt|imx93frdm|-rt"
)

for t in "${TARGETS[@]}"; do
    IFS='|' read -r tree machine suffix <<< "$t"
    img="$tree/build_lsdk2606/images"
    final="$OUT/sdcard_${machine}${suffix}.wic.zst"

    if [ -f "$final" ]; then
        echo "=== $(basename "$final") already exists, skipping ==="
        continue
    fi

    echo
    echo "############ mkwic $machine${suffix:+ ($suffix)} ############"
    echo "tree=$tree"

    for f in "$img/${machine}-sd-flash.bin" "$img/$BOOT" \
             "$img/rootfs_lsdk2606_debian_${machine}.tar.zst"; do
        [ -f "$f" ] || { echo "!!! missing $f -- skipping $machine$suffix"; continue 2; }
    done

    docker run --rm --privileged --net=host \
        -v "$tree":"$tree" -v /dev:/dev -w "$tree" "$IMAGE" \
        bash -c "tools/flex-installer -i mkwic -m $machine \
            -f build_lsdk2606/images/${machine}-sd-flash.bin \
            -b build_lsdk2606/images/$BOOT \
            -r build_lsdk2606/images/rootfs_lsdk2606_debian_${machine}.tar.zst"
    rc=$?

    produced="$tree/rootfs_lsdk2606_debian_${machine}.wic.zst"
    if [ "$rc" -ne 0 ] || [ ! -f "$produced" ]; then
        echo "!!! mkwic FAILED for $machine$suffix (rc=$rc)"
        continue
    fi

    mv -f "$produced" "$final"
    echo "=== $final ($(du -h "$final" | cut -f1)) ==="
    ( cd "$OUT" && sha256sum "$(basename "$final")" > "$(basename "$final").sha256" )
done

echo
echo "################ SD IMAGES $(date -Is) ################"
ls -lh "$OUT"/*.wic.zst 2>/dev/null || echo "  (none)"
