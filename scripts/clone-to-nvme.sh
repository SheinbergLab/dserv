#!/usr/bin/env bash
#
# clone-to-nvme.sh - mirror the running Raspberry Pi system onto a second disk
#                    (normally the NVMe), and keep it refreshable.
#
# First run needs --init: it formats the target's two partitions, then copies.
# Every run after that is incremental - rsync only moves what changed, so a
# refresh takes seconds to a couple of minutes instead of a full copy.
#
# The target's partition TABLE is never touched, so its PARTUUIDs survive both
# --init and every resync. That matters: the copied /etc/fstab and cmdline.txt
# are rewritten to those PARTUUIDs, so the clone is bootable on its own and
# does not fight the source disk for identity.
#
# Boot order on a Pi 5 is set in the EEPROM (BOOT_ORDER). With the stock
# 0xf461 the firmware tries SD first and falls through to NVMe, so pulling the
# SD is all that is needed to boot the clone. Check with: rpi-eeprom-config
#
# Usage:
#   sudo ./clone-to-nvme.sh --init                    # first time (DESTRUCTIVE)
#   sudo ./clone-to-nvme.sh                           # incremental resync
#   sudo ./clone-to-nvme.sh --dry-run                 # show what would change
#   sudo ./clone-to-nvme.sh --stop "dserv dserv-agent"
#
# Options:
#   --target DEV   disk to clone onto            (default: /dev/nvme0n1)
#   --init         mkfs the target first         (DESTROYS all data on it)
#   --dry-run      rsync -n; no writes at all
#   --stop "LIST"  stop these services during the copy, restart after
#   --yes          skip the interactive confirmation for --init
#
set -euo pipefail

TARGET=/dev/nvme0n1
MNT=/mnt/clone-target
INIT=0
DRYRUN=0
ASSUME_YES=0
STOP_SERVICES=""

die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s\n' "$*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --target)  TARGET="${2:-}"; shift 2 ;;
    --init)    INIT=1; shift ;;
    --dry-run) DRYRUN=1; shift ;;
    --yes)     ASSUME_YES=1; shift ;;
    --stop)    STOP_SERVICES="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *)         die "unknown option: $1" ;;
  esac
done

[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
[ -b "$TARGET" ]     || die "$TARGET is not a block device"

# ---------------------------------------------------------------- discovery

SRC_ROOT_DEV=$(findmnt -no SOURCE /)
SRC_BOOT_DEV=$(findmnt -no SOURCE /boot/firmware) \
  || die "/boot/firmware is not mounted; cannot clone the boot partition"
SRC_DISK=/dev/$(lsblk -no PKNAME "$SRC_ROOT_DEV")

[ "$SRC_DISK" != "$TARGET" ] \
  || die "target $TARGET is the disk we are currently running from"

# Read the target's partitions rather than assuming a "p1"/"p2" suffix, which
# differs between nvme0n1p1 and sda1.
mapfile -t TGT_PARTS < <(lsblk -lnpo NAME,TYPE "$TARGET" | awk '$2=="part"{print $1}')
[ "${#TGT_PARTS[@]}" -eq 2 ] \
  || die "expected 2 partitions on $TARGET, found ${#TGT_PARTS[@]} - partition it first (512M vfat + ext4)"

TGT_BOOT="${TGT_PARTS[0]}"
TGT_ROOT="${TGT_PARTS[1]}"
TGT_BOOT_PARTUUID=$(blkid -s PARTUUID -o value "$TGT_BOOT") || die "no PARTUUID on $TGT_BOOT"
TGT_ROOT_PARTUUID=$(blkid -s PARTUUID -o value "$TGT_ROOT") || die "no PARTUUID on $TGT_ROOT"

# The rewrite below keys off PARTUUID=; anything else means this system
# identifies its disks some other way and the clone would silently misboot.
grep -q 'PARTUUID=' /etc/fstab \
  || die "/etc/fstab does not use PARTUUID= - rewrite logic would not apply"

SRC_USED_KB=$(df -kP / | awk 'NR==2{print $3}')
TGT_ROOT_KB=$(( $(blockdev --getsize64 "$TGT_ROOT") / 1024 ))
[ "$TGT_ROOT_KB" -gt "$SRC_USED_KB" ] \
  || die "target root ($((TGT_ROOT_KB/1024))M) is smaller than data in use ($((SRC_USED_KB/1024))M)"

say "source: $SRC_ROOT_DEV (/) + $SRC_BOOT_DEV (/boot/firmware), $((SRC_USED_KB/1024))M in use"
say "target: $TGT_ROOT (PARTUUID=$TGT_ROOT_PARTUUID) + $TGT_BOOT (PARTUUID=$TGT_BOOT_PARTUUID)"

if [ "$INIT" -eq 1 ] && [ "$DRYRUN" -eq 0 ] && [ "$ASSUME_YES" -eq 0 ]; then
  say "--init will ERASE every existing filesystem on $TARGET:"
  lsblk -o NAME,SIZE,FSTYPE,LABEL "$TARGET"
  printf '\nType ERASE to continue: '
  read -r reply
  [ "$reply" = "ERASE" ] || die "aborted"
fi

# ------------------------------------------------------------------ cleanup

restart_services() {
  [ -n "$STOP_SERVICES" ] || return 0
  say "restarting: $STOP_SERVICES"
  # shellcheck disable=SC2086
  systemctl start $STOP_SERVICES || true
}

cleanup() {
  umount "$MNT/boot/firmware" 2>/dev/null || true
  umount "$MNT"               2>/dev/null || true
  rmdir  "$MNT"               2>/dev/null || true
  restart_services
}
trap cleanup EXIT

if [ -n "$STOP_SERVICES" ] && [ "$DRYRUN" -eq 0 ]; then
  say "stopping: $STOP_SERVICES"
  # shellcheck disable=SC2086
  systemctl stop $STOP_SERVICES
  sleep 2   # let sqlite checkpoint and close
elif [ "$DRYRUN" -eq 0 ] && systemctl is-active --quiet dserv.service 2>/dev/null; then
  say "note: dserv.service is running and writing sqlite live."
  echo "     Its databases may copy mid-write. Consider:"
  echo "       --stop \"dserv dserv-agent\""
fi

# -------------------------------------------------------------------- mkfs

if [ "$INIT" -eq 1 ]; then
  if [ "$DRYRUN" -eq 1 ]; then
    say "dry run: would mkfs.vfat $TGT_BOOT and mkfs.ext4 $TGT_ROOT"
  else
    say "formatting $TGT_BOOT (vfat) and $TGT_ROOT (ext4)"
    # Labels match stock Raspberry Pi OS. mkfs does not alter the partition
    # table, so the PARTUUIDs read above stay valid.
    mkfs.vfat -F 32 -n bootfs "$TGT_BOOT"
    mkfs.ext4 -F -L rootfs "$TGT_ROOT"
  fi
fi

# ------------------------------------------------------------------- mount

mkdir -p "$MNT"
mount "$TGT_ROOT" "$MNT"
mkdir -p "$MNT/boot/firmware"
mount "$TGT_BOOT" "$MNT/boot/firmware"

# --------------------------------------------------------------------- copy

RSYNC_ROOT_OPTS=(-aHAXx --delete --numeric-ids)
RSYNC_BOOT_OPTS=(-rltD --delete --modify-window=2)

# progress2 repaints with \r, which is fine on a terminal but turns into
# hundreds of thousands of lines when piped to a log or run over ssh.
if [ -t 1 ]; then
  RSYNC_ROOT_OPTS+=(--info=progress2)
  RSYNC_BOOT_OPTS+=(--info=progress2)
else
  RSYNC_ROOT_OPTS+=(--info=stats2)
  RSYNC_BOOT_OPTS+=(--info=stats2)
fi

[ "$DRYRUN" -eq 1 ] && { RSYNC_ROOT_OPTS+=(-n); RSYNC_BOOT_OPTS+=(-n); }

# -x already stops rsync descending into other filesystems, so /proc, /sys,
# /dev, /run and /tmp are skipped as content but still created as empty
# mountpoints. The excludes below are for things on the root fs itself.
say "copying / -> $MNT"
rsync "${RSYNC_ROOT_OPTS[@]}" \
  --exclude="$MNT" \
  --exclude=/boot/firmware/'*' \
  --exclude=/lost+found \
  --exclude=/swapfile \
  --exclude=/var/swap \
  --exclude=/var/tmp/'*' \
  --exclude=/var/cache/apt/archives/'*.deb' \
  / "$MNT/"

say "copying /boot/firmware -> $MNT/boot/firmware"
rsync "${RSYNC_BOOT_OPTS[@]}" /boot/firmware/ "$MNT/boot/firmware/"

if [ "$DRYRUN" -eq 1 ]; then
  say "dry run complete - nothing was written"
  exit 0
fi

# Empty mountpoints the kernel needs at boot; rsync creates most, but be sure.
for d in proc sys dev run tmp mnt media; do mkdir -p "$MNT/$d"; done
chmod 1777 "$MNT/tmp"

# --------------------------------------------------- point the clone at itself

say "rewriting fstab and cmdline.txt to the target PARTUUIDs"

sed -i -E \
  -e "s|PARTUUID=[0-9a-fA-F]{8}-01|PARTUUID=${TGT_BOOT_PARTUUID}|g" \
  -e "s|PARTUUID=[0-9a-fA-F]{8}-02|PARTUUID=${TGT_ROOT_PARTUUID}|g" \
  "$MNT/etc/fstab"

sed -i -E \
  -e "s|root=PARTUUID=[0-9a-fA-F]{8}-02|root=PARTUUID=${TGT_ROOT_PARTUUID}|g" \
  "$MNT/boot/firmware/cmdline.txt"

# Verify, rather than trust the sed: a clone that still names the source disk
# boots the SD and looks like success until the SD is pulled.
grep -q "$TGT_ROOT_PARTUUID" "$MNT/etc/fstab" \
  || die "fstab rewrite failed - clone would not boot standalone"
grep -q "$TGT_ROOT_PARTUUID" "$MNT/boot/firmware/cmdline.txt" \
  || die "cmdline.txt rewrite failed - clone would not boot standalone"

if grep -qE "PARTUUID=[0-9a-fA-F]{8}" "$MNT/etc/fstab" \
   && grep -vqE "PARTUUID=(${TGT_BOOT_PARTUUID}|${TGT_ROOT_PARTUUID})" \
        <(grep -oE "PARTUUID=[0-9a-fA-F]{8}-[0-9]+" "$MNT/etc/fstab"); then
  say "WARNING: target fstab still references a foreign PARTUUID:"
  grep -nE "PARTUUID=" "$MNT/etc/fstab"
fi

sync

say "target fstab:"
grep -vE '^\s*#' "$MNT/etc/fstab" | grep -vE '^\s*$'
say "target cmdline.txt:"
cat "$MNT/boot/firmware/cmdline.txt"

say "done - $TARGET is a bootable mirror of the running system"
echo "     Resync later with: sudo $0${STOP_SERVICES:+ --stop \"$STOP_SERVICES\"}"
echo "     To boot from it:   shut down, remove the SD card, power on."
