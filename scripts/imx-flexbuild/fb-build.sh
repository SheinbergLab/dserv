#!/bin/bash
# fb-build.sh <machine> [<machine> ...]
#
# Detached flexbuild driver for the NXP Debian Linux SDK.
#
# flexbuild's `make docker` is interactive-only (docker run -it ... /bin/bash), so
# this replicates the same container contract -- privileged, net=host, with $FBDIR,
# /dev and /lib/modules mounted -- but runs the build as the container command.
# Safe to launch under tmux and walk away.
#
# Machines build one at a time: .config is global to the tree.
#
#   ./fb-build.sh imx93frdm imx95-15x15-frdm
#   FBDIR=/data/flexbuild-rt EXTRA_KCONFIG=rt.config \
#     VERIFY_KCONFIG=CONFIG_PREEMPT_RT DISABLE_ML=1 ./fb-build.sh imx93frdm
#
# Env:
#   FBDIR          tree to build in                    (default /data/flexbuild)
#   EXTRA_KCONFIG  kernel fragment in configs/linux/   (must end in .config)
#   VERIFY_KCONFIG symbol that MUST be set in the generated kernel .config
#   DISABLE_ML=1   turn off CONFIG_APP_ML (TensorFlow Lite / Ethos-U / NNStreamer)
#
# Logs:   $FBDIR/logs/build_<machine>_<stamp>.log
# Status: $FBDIR/logs/<machine>.status   ("running" -> "ok" | "FAILED rc=N")

set -uo pipefail

FBDIR="${FBDIR:-/data/flexbuild}"
IMAGE=fbdebian:13-flexbuild
EXTRA_KCONFIG="${EXTRA_KCONFIG:-}"
VERIFY_KCONFIG="${VERIFY_KCONFIG:-}"
DISABLE_ML="${DISABLE_ML:-0}"
MACHINES=("${@:?usage: fb-build.sh <machine> [<machine> ...]}")

mkdir -p "$FBDIR/logs"

# ---------------------------------------------------------------------------
# arm64 binfmt handler.  The rootfs stage debootstraps arm64 and chroots into
# it, which needs a handler on the *host kernel*; pogo has no qemu-user-static
# and sudo needs a password.  So register from inside the privileged container
# using Debian's own static qemu.  The `F` (fix-binary) flag is the point: the
# kernel opens the interpreter at registration time and holds that fd, so it
# survives both the chroot and this container exiting.  Kernel-global, and NOT
# preserved across a reboot -- hence the check on every run.
# ---------------------------------------------------------------------------
ensure_binfmt() {
    if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        echo "=== binfmt: qemu-aarch64 already registered ==="
        return 0
    fi
    echo "=== binfmt: registering qemu-aarch64 (host kernel, lost on reboot) ==="
    docker run --rm --privileged "$IMAGE" /bin/bash -c '
        mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
        [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] && exit 0
        echo ":qemu-aarch64:M::\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:F" \
            > /proc/sys/fs/binfmt_misc/register
    '
    if [ "$(docker run --rm --platform linux/arm64 debian:trixie uname -m 2>/dev/null)" = "aarch64" ]; then
        echo "=== binfmt: verified (arm64 executes) ==="
    else
        echo "!!! binfmt: arm64 still not executable -- rootfs stage will fail" >&2
        return 1
    fi
}

in_container() {
    docker run --rm \
        --hostname fbdebian --add-host fbdebian:127.0.0.1 \
        --privileged --net=host \
        -v "$HOME":"$HOME" -v "$FBDIR":"$FBDIR" \
        -v /lib/modules:/lib/modules -v /dev:/dev \
        -v /etc/localtime:/etc/localtime:ro \
        -w "$FBDIR" "$IMAGE" /bin/bash -c "$1"
}

# Every `make <machine>_defconfig` regenerates .config from scratch, wiping any
# edits -- so these fixups must be reapplied after EVERY defconfig, not once.
post_config_cmds() {
    local c=""
    # Pin JOBS to 24 -- a flexbuild bug that only bites on >24-core hosts.
    # Makefile does `MAKEFLAGS += -j$(CONFIG_JOBS)`; kconfig_hooks.py rewrites a
    # 0 to multiprocessing.cpu_count(), but Kconfig declares `range 0 24`, so on
    # this 32-core box kconfiglib rejects 32 ("outside the active range") and
    # falls back to the default 0 -> `-j0` -> "the '-j' option requires a
    # positive integer argument" on every sub-make.  24 is the cap and is
    # preserved, because the hook only rewrites JOBS when it is 0.
    c+="sed -i 's|^CONFIG_JOBS=.*|CONFIG_JOBS=24|' .config && "
    if [ -n "$EXTRA_KCONFIG" ]; then
        c+="sed -i 's|^CONFIG_LINUX_EXTRA_CONFIG=.*|CONFIG_LINUX_EXTRA_CONFIG=\"$EXTRA_KCONFIG\"|' .config && "
    fi
    if [ "$DISABLE_ML" = 1 ]; then
        # TensorFlow Lite fails to cross-compile and aborts `make all` at the
        # apps stage -- before flash.bin/boot/merge-apps/packrfs -- so the build
        # yields no bootable image at all.  None of the ML stack is needed here.
        c+="sed -i 's|^CONFIG_APP_ML=y|# CONFIG_APP_ML is not set|' .config && "
    fi
    [ -n "$c" ] && c+="python3 tools/kconfig_hooks.py && "
    printf '%s' "$c"
}

build_one() {
    local machine="$1" stamp log rc rcfile post kcfg
    stamp=$(date +%Y%m%d_%H%M%S)
    log="$FBDIR/logs/build_${machine}_${stamp}.log"
    rcfile=$(mktemp)
    post=$(post_config_cmds)

    echo "running" > "$FBDIR/logs/${machine}.status"

    {
        echo "=== machine=$machine start=$(date -Is) host=$(hostname) ==="
        echo "=== FBDIR=$FBDIR extra=${EXTRA_KCONFIG:-none} disable_ml=$DISABLE_ML ==="
        echo "=== flexbuild HEAD: $(cd "$FBDIR" && git log -1 --format='%h %ad %s' --date=short) ==="

        # Prove the kernel fragment took BEFORE the long build.  linux.mk only
        # generates the kernel .config `if [ ! -f $(KOUTDIR)/.config ]` and sends
        # that step to /dev/null, so a failed merge is otherwise silent.
        if [ -n "$EXTRA_KCONFIG" ] && [ -n "$VERIFY_KCONFIG" ]; then
            in_container "set -e; make ${machine}_defconfig && ${post}make dl-kernel" \
                || { echo "!!! kernel config/fetch failed"; echo 1 > "$rcfile"; }
            if [ ! -s "$rcfile" ]; then
                kcfg=$(ls "$FBDIR"/build_*/linux/linux/arm64/*/output/*/.config 2>/dev/null | head -1)
                if [ -n "$kcfg" ] && grep -q "^${VERIFY_KCONFIG}=y" "$kcfg"; then
                    echo "=== OK: ${VERIFY_KCONFIG}=y in $kcfg ==="
                    grep -E '^CONFIG_PREEMPT' "$kcfg" || true
                else
                    echo "!!! ${VERIFY_KCONFIG} NOT set -- aborting before the long build" >&2
                    echo 1 > "$rcfile"
                fi
            fi
        fi

        if [ ! -s "$rcfile" ]; then
            in_container "set -o pipefail; make ${machine}_defconfig && ${post}make all"
            echo $? > "$rcfile"
        fi

        echo "=== machine=$machine end=$(date -Is) ==="
    } 2>&1 | tee "$log"

    # NOT ${PIPESTATUS[0]}: that is the brace group's status, i.e. the trailing
    # echo, which is always 0 -- it silently reported failed builds as "ok".
    rc=$(cat "$rcfile" 2>/dev/null || echo 1); rm -f "$rcfile"
    [ -n "$rc" ] || rc=1

    if [ "$rc" -eq 0 ]; then
        echo "ok" > "$FBDIR/logs/${machine}.status"; echo ">>> $machine: OK   log=$log"
    else
        echo "FAILED rc=$rc" > "$FBDIR/logs/${machine}.status"; echo ">>> $machine: FAILED rc=$rc   log=$log"
    fi
    return "$rc"
}

# ---------------------------------------------------------------------------

docker image inspect "$IMAGE" >/dev/null 2>&1 || \
    make -C "$FBDIR/docker/debian" IMAGE_TAG="$IMAGE" build || exit 1

ensure_binfmt || exit 1

overall=0
for m in "${MACHINES[@]}"; do build_one "$m" || overall=1; done

echo
echo "================ SUMMARY $(date -Is) ================"
echo "FBDIR=$FBDIR"
for m in "${MACHINES[@]}"; do
    printf '  %-24s %s\n' "$m" "$(cat "$FBDIR/logs/${m}.status" 2>/dev/null || echo '?')"
done
echo "Artifacts:"
ls -lh "$FBDIR"/build_*/images/ 2>/dev/null || echo "  (none)"
exit "$overall"
