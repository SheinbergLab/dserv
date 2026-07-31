#!/bin/bash
#
# imx-sdcard.sh -- fetch NXP i.MX Debian SD images from the build host and
#                  flash them to an SD card on macOS.
#
# Images are built on the build host by flexbuild (/data/fb-build.sh) and
# assembled into all-in-one SD images by /data/fb-mkwic.sh.  That split is the
# point: partitioning and mkfs.ext4 only work on Linux, so flex-installer does
# all of it there and produces ONE compressed image per board.  macOS then only
# has to do a raw block write -- no partitioning, no ext4 support needed.
#
#   ./imx-sdcard.sh list                    # what's available, remote and local
#   ./imx-sdcard.sh fetch                   # get all images
#   ./imx-sdcard.sh fetch imx93             # get matching ones (substring)
#   ./imx-sdcard.sh flash sdcard_imx93frdm.wic.zst        # pick disk interactively
#   ./imx-sdcard.sh flash sdcard_imx93frdm.wic.zst disk4
#
# Images (~1.4 GB compressed, 10 GB written -- use a 16 GB or larger card):
#   sdcard_imx93frdm.wic.zst           FRDM-IMX93,       stock kernel
#   sdcard_imx93frdm-rt.wic.zst        FRDM-IMX93,       PREEMPT_RT kernel
#   sdcard_imx95-15x15-frdm.wic.zst    FRDM-IMX95 15x15, stock kernel
#
# Env:
#   IMX_HOST   build host             (default pogo.neuro.brown.edu)
#   IMX_REMOTE remote image directory (default /data/imx-sdcards)
#   IMX_DEST   local image directory  (default ~/imx-images)
#
# Requires: rsync, zstd. Optional: pv (progress bar). Flashing needs sudo.

set -euo pipefail

IMX_HOST="${IMX_HOST:-pogo.neuro.brown.edu}"
IMX_REMOTE="${IMX_REMOTE:-/data/imx-sdcards}"
IMX_DEST="${IMX_DEST:-$HOME/imx-images}"

red() { printf '\033[31m%s\033[0m\n' "$*"; }
grn() { printf '\033[32m%s\033[0m\n' "$*"; }
ylw() { printf '\033[33m%s\033[0m\n' "$*"; }
die() { red "error: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found${2:+ -- $2}"; }

# ---------------------------------------------------------------- list

cmd_list() {
    echo "Remote ($IMX_HOST:$IMX_REMOTE):"
    ssh "$IMX_HOST" "ls -lh $IMX_REMOTE/*.wic.zst 2>/dev/null" 2>/dev/null \
        | awk '{printf "  %-8s %s\n", $5, $NF}' || true
    echo
    echo "Local ($IMX_DEST):"
    ls -lh "$IMX_DEST"/*.wic.zst 2>/dev/null | awk '{printf "  %-8s %s\n", $5, $NF}' \
        || echo "  (none yet)"
}

# ---------------------------------------------------------------- fetch

cmd_fetch() {
    local filter="${1:-}"
    need rsync
    mkdir -p "$IMX_DEST"

    local files
    files=$(ssh "$IMX_HOST" "ls $IMX_REMOTE/*.wic.zst 2>/dev/null" 2>/dev/null || true)
    [ -n "$files" ] || die "no images on $IMX_HOST:$IMX_REMOTE"

    local n=0
    while IFS= read -r rf; do
        [ -z "$rf" ] && continue
        local base; base=$(basename "$rf")
        [ -n "$filter" ] && [[ "$base" != *"$filter"* ]] && continue

        echo
        grn "fetching $base"
        # --partial --inplace so an interrupted transfer resumes rather than
        # restarting -- these are ~1.4 GB over a VPN from a remote site.
        rsync -h --progress --partial --inplace \
            "$IMX_HOST:$rf" "$IMX_HOST:$rf.sha256" "$IMX_DEST/" 2>/dev/null \
          || rsync -h --progress --partial --inplace "$IMX_HOST:$rf" "$IMX_DEST/"
        n=$((n+1))

        if [ -f "$IMX_DEST/$base.sha256" ]; then
            printf '  verifying... '
            if ( cd "$IMX_DEST" && shasum -a 256 -c "$base.sha256" >/dev/null 2>&1 ); then
                grn "sha256 OK"
            else
                red "sha256 MISMATCH for $base -- do not flash this"
            fi
        fi
    done <<< "$files"

    [ "$n" -eq 0 ] && die "nothing matched${filter:+ '$filter'} -- try: $0 list"
    echo; grn "$n image(s) in $IMX_DEST"
}

# ---------------------------------------------------------------- flash

cmd_flash() {
    local img="${1:-}" disk="${2:-}"
    [ -n "$img" ] || die "usage: $0 flash <image.wic.zst> [diskN]"
    [ -f "$img" ] || img="$IMX_DEST/$img"
    [ -f "$img" ] || die "image not found: ${1}"
    need zstd "brew install zstd"

    # Verify before writing, if we have the checksum -- a corrupt 10 GB write
    # that fails at boot is a miserable thing to debug.
    local sha="$img.sha256"
    if [ -f "$sha" ]; then
        printf 'verifying image checksum... '
        ( cd "$(dirname "$img")" && shasum -a 256 -c "$(basename "$sha")" >/dev/null 2>&1 ) \
            && grn "OK" || die "sha256 mismatch on $(basename "$img") -- re-fetch it"
    else
        ylw "no .sha256 alongside image; skipping verification"
    fi

    if [ -z "$disk" ]; then
        echo; ylw "External physical disks:"
        diskutil list external physical || true
        echo
        printf 'Target disk identifier (e.g. disk4), or Ctrl-C to abort: '
        read -r disk
    fi
    disk="${disk#/dev/}"
    [[ "$disk" =~ ^disk[0-9]+$ ]] || die "expected a disk identifier like 'disk4', got '$disk'"

    local info
    info=$(diskutil info "/dev/$disk" 2>/dev/null) || die "no such disk: /dev/$disk"

    local location size devname protocol removable disk_bytes
    location=$(awk -F': *' '/Device Location/{print $2; exit}' <<<"$info" | xargs || true)
    size=$(sed -n 's/.*Disk Size: *\([0-9.]* [KMGT]B\).*/\1/p' <<<"$info" | head -1)
    devname=$(awk -F': *' '/Device \/ Media Name/{print $2; exit}' <<<"$info" | xargs || true)
    protocol=$(awk -F': *' '/Protocol/{print $2; exit}' <<<"$info" | xargs || true)
    removable=$(awk -F': *' '/Removable Media/{print $2; exit}' <<<"$info" | xargs || true)
    disk_bytes=$(sed -n 's/.*Disk Size:.*(\([0-9][0-9]*\) Bytes).*/\1/p' <<<"$info" | head -1)

    # Hard refusal, not a warning: this writes a partition table to the whole
    # device and there is no undo.
    [ "$location" = "Internal" ] && die "/dev/$disk is an INTERNAL disk -- refusing"

    # Decompressed image size, used for both the capacity check and pv.
    local img_bytes
    img_bytes=$(zstd -lv "$img" 2>/dev/null | sed -n 's/.*Decompressed Size:.*(\([0-9][0-9]*\) B).*/\1/p')

    # An undersized card is the failure that wastes the most time: dd dies
    # partway and leaves a card that looks written but cannot boot.
    if [ -n "$img_bytes" ] && [ -n "$disk_bytes" ] && [ "$disk_bytes" -lt "$img_bytes" ]; then
        die "/dev/$disk is too small: $((disk_bytes/1000/1000)) MB capacity vs $((img_bytes/1000/1000)) MB image"
    fi

    echo
    ylw "About to ERASE and overwrite:"
    printf '  device    : /dev/%s\n  name      : %s\n  size      : %s\n  location  : %s (%s)\n  removable : %s\n  image     : %s%s\n' \
        "$disk" "${devname:-?}" "${size:-?}" "${location:-?}" "${protocol:-?}" \
        "${removable:-?}" "$img" \
        "${img_bytes:+ ($((img_bytes/1000/1000)) MB written)}"

    # "External" alone is a weak guarantee -- an external backup SSD is also
    # External. Removable media (a card reader) reports "Removable"; a fixed
    # external drive reports "Fixed". Don't refuse it outright (some readers
    # do report Fixed), but make the user say so out loud.
    local phrase="$disk"
    if [ "$removable" != "Removable" ]; then
        echo
        red "WARNING: /dev/$disk is NOT removable media (Removable Media: ${removable:-unknown})."
        red "An external hard drive or SSD looks like this. An SD card reader usually reports 'Removable'."
        phrase="erase $disk"
    fi

    echo
    red "This destroys everything on /dev/$disk."
    printf "Type '%s' to confirm: " "$phrase"
    local confirm; read -r confirm
    [ "$confirm" = "$phrase" ] || die "confirmation did not match -- aborted"

    echo; echo "caching sudo credentials..."; sudo -v
    echo "unmounting /dev/$disk ..."
    diskutil unmountDisk "/dev/$disk"

    # /dev/rdiskN is the raw, unbuffered node -- substantially faster than the
    # buffered /dev/diskN for a bulk sequential write.
    echo "writing to /dev/r$disk ..."
    if command -v pv >/dev/null 2>&1; then
        # img_bytes came from `zstd -lv` above: exact bytes, because pv rejects
        # the "10 GiB" form that plain `zstd -l` prints. (sed, not awk match(),
        # because BSD awk lacks match() with an array.)
        if [ -n "$img_bytes" ]; then
            zstd -dc "$img" | pv -s "$img_bytes" | sudo dd of="/dev/r$disk" bs=4m
        else
            zstd -dc "$img" | pv | sudo dd of="/dev/r$disk" bs=4m
        fi
    else
        echo "(no pv -- 'brew install pv' for a progress bar; Ctrl-T for status)"
        zstd -dc "$img" | sudo dd of="/dev/r$disk" bs=4m
    fi

    sync
    echo; grn "write complete; ejecting"
    diskutil eject "/dev/$disk" || true
    grn "done -- card ready for the board"
}

# ----------------------------------------------------------------

case "${1:-}" in
    list)  shift; cmd_list ;;
    fetch) shift; cmd_fetch "${1:-}" ;;
    flash) shift; cmd_flash "$@" ;;
    *) sed -n '3,31p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
