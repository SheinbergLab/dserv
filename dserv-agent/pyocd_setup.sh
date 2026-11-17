# pyocd_setup.sh -- flash extio firmware onto an FRDM board through its onboard
# debug probe (body of /extio/flash for the pyocd boards, e.g. frdm_mcxn947).
#
# Served behind a preamble written by extio_flash.go that sets, for the image
# the registry picked: BOARD_NAME BOARD_TITLE FW_SHELF_URL FW_CHANNEL FW_VERSION
# FW_IMGVER FW_PUBLISHED HEX_URL HEX_NAME HEX_SHA256 HEX_SIZE, and the pinned
# tools: PYOCD_TARGET PYOCD_VERSION PACK_URL PACK_NAME PACK_SHA256.
#
#   curl -sSL "https://dserv.net/extio/flash?board=frdm_mcxn947" | bash
#   curl -sSL "https://dserv.net/extio/flash?board=frdm_mcxn947" | bash -s -- --dry-run
#
# SELF-CONTAINED ON PURPOSE. pyOCD goes into a venv of its own under
# ~/.extio (EXTIO_HOME), the device pack is the pinned one checked by sha256,
# and pyOCD runs with --no-config from a temp dir -- so a Zephyr install, a
# pyocd.yaml lying in the current directory, or packs in ~/.pyocd can neither
# help nor hurt. What this flashes is the same on a developer's Mac and a
# machine that has never seen Zephyr.
#
# Everything is inside main(), called on the last line, so bash has read the
# whole script before anything runs. Under curl|bash the script IS stdin, and a
# child that reads stdin (pip, apt-get) would otherwise swallow the rest of it.

set -euo pipefail

say()  { printf '>> %s\n' "$*" >&2; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() {
  cat >&2 <<EOF
Flash extio firmware onto an ${BOARD_TITLE:-FRDM board} through its onboard MCU-Link.

  curl -sSL "${FW_SHELF_URL:-https://dserv.net}/extio/flash?board=${BOARD_NAME:-frdm_mcxn947}" | bash [-s -- OPTIONS]

URL parameters choose the image:  board=${BOARD_NAME:-frdm_mcxn947}  channel=dev  version=<v>

Options:
  --dry-run       set up pyOCD, download and check everything, look for the
                  board -- but flash nothing
  --probe UID     which probe, when more than one is plugged in
  --no-install    do not set up pyOCD if it is missing; just stop
  -h, --help      this text

pyOCD $PYOCD_VERSION is installed into ${EXTIO_HOME:-~/.extio} (set EXTIO_HOME to move it),
not into any Python you already have.

This is the FIRST flash: it writes MCUboot and the app together, so it is what
a blank board needs. A board already running extio firmware does not need it
to update -- its dserv can update it over the wire:
  dservctl extio "extio_ota_push_shelf <box> ${FW_CHANNEL:-dev}"
EOF
}

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else die "need shasum or sha256sum to check the downloads"; fi
}

# The firmware's USB product string (extio-zephyr/src/platform/box_usbd.c).
# On the FRDM boards it is on the MCU's OWN USB port, not the MCU-Link's.
extio_box_present() {
  case "$(uname -s)" in
    Darwin) ioreg -p IOUSB -w0 2>/dev/null | grep -q 'extio USB box' ;;
    Linux)  grep -qx 'extio USB box' /sys/bus/usb/devices/*/product 2>/dev/null ;;
    *)      return 1 ;;
  esac
}

# A python3 new enough for pyOCD, with venv + ensurepip. Debian splits ensurepip
# into python3-venv, and a venv made without it has no pip.
python_ok() {
  command -v python3 >/dev/null 2>&1 &&
    python3 -c 'import sys, venv, ensurepip; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1
}

need_python() {
  python_ok && return 0
  [ "$noinstall" = 1 ] && die "pyOCD needs python3 (3.9+) with venv support, and none was found."
  case "$(uname -s)" in
    Darwin)
      die "pyOCD needs python3 (3.9+). On macOS run   xcode-select --install
   (the Command Line Tools include python3), or install python from python.org
   or Homebrew, then re-run." ;;
    Linux)
      command -v apt-get >/dev/null 2>&1 || die "pyOCD needs python3 (3.9+) with the venv module; install them and re-run."
      say "installing python3-venv (apt-get; asks for your password)"
      $SUDO apt-get install -y python3 python3-venv </dev/null
      python_ok || die "python3 with venv support is still missing after installing python3-venv" ;;
    *)
      die "unsupported system $(uname -s)" ;;
  esac
}

# pyOCD in a venv of its own, reused when it is already the pinned version.
setup_pyocd() {
  local venv="$1"
  PYOCD="$venv/bin/pyocd"
  PYOCD_PY="$venv/bin/python"
  if [ -x "$PYOCD" ] && [ "$("$PYOCD" --version 2>/dev/null </dev/null)" = "$PYOCD_VERSION" ]; then
    say "pyOCD   $PYOCD_VERSION ($venv)"
    return 0
  fi
  [ "$noinstall" = 1 ] && die "pyOCD $PYOCD_VERSION is not set up in $venv (re-run without --no-install)"
  need_python
  say "setting up pyOCD $PYOCD_VERSION in $venv (once; about a minute)"
  rm -rf "$venv"
  mkdir -p "$(dirname "$venv")"
  python3 -m venv "$venv" </dev/null || die "could not create a python venv in $venv"
  "$venv/bin/python" -m pip install -q --disable-pip-version-check "pyocd==$PYOCD_VERSION" </dev/null \
    || { rm -rf "$venv"; die "pip could not install pyocd==$PYOCD_VERSION (network? see the pip output above)"; }
  [ "$("$PYOCD" --version 2>/dev/null </dev/null)" = "$PYOCD_VERSION" ] \
    || { rm -rf "$venv"; die "pyOCD installed but does not report version $PYOCD_VERSION"; }
  say "pyOCD   $PYOCD_VERSION ($venv)"
}

# The pinned device pack, cached; a cached copy is re-checked every run.
fetch_pack() {
  PACK="$1/$PACK_NAME"
  if [ -f "$PACK" ] && [ "$(sha256_of "$PACK")" = "$PACK_SHA256" ]; then
    say "pack    $PACK_NAME (cached)"
    return 0
  fi
  mkdir -p "$1"
  say "downloading the device pack $PACK_NAME (~10 MB, once)"
  curl -fsSL --retry 2 -o "$PACK.part" "$PACK_URL" </dev/null || die "could not download $PACK_URL"
  local got
  got=$(sha256_of "$PACK.part")
  [ "$got" = "$PACK_SHA256" ] || { rm -f "$PACK.part"; die "device pack sha256 mismatch -- NOT using it.
   expected $PACK_SHA256
   got      $got"; }
  mv "$PACK.part" "$PACK"
  say "pack    $PACK_NAME (sha256 OK)"
}

main() {
  local dry=0 probe=""
  noinstall=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --dry-run)    dry=1 ;;
      --no-install) noinstall=1 ;;
      --probe)      [ $# -ge 2 ] || die "--probe needs a probe unique ID"; probe="$2"; shift ;;
      --probe=*)    probe="${1#--probe=}" ;;
      -h|--help)    usage; exit 0 ;;
      *)            usage; die "unknown option: $1" ;;
    esac
    shift
  done

  : "${HEX_URL:?no image URL -- run this through ${FW_SHELF_URL:-https://dserv.net}/extio/flash?board=...}"

  case "$(uname -s)" in
    Darwin|Linux) ;;
    *) die "unsupported system $(uname -s). On Windows, install pyOCD $PYOCD_VERSION and flash
   $HEX_URL
   with:  pyocd flash --no-config -t $PYOCD_TARGET --pack $PACK_NAME <the .hex>" ;;
  esac

  # On Linux the MCU-Link is only reachable as root unless a udev rule grants
  # it, so pyOCD's probe access runs under sudo. Installing pyOCD does not.
  SUDO=""
  if [ "$(uname -s)" = Linux ] && [ "$(id -u)" != 0 ]; then
    command -v sudo >/dev/null 2>&1 && SUDO=sudo
  fi

  local home="${EXTIO_HOME:-$HOME/.extio}"

  say "extio firmware for an $BOARD_TITLE"
  say "  image   $HEX_NAME  ($FW_CHANNEL $FW_VERSION${FW_IMGVER:+, firmware $FW_IMGVER})"
  say "  from    $FW_SHELF_URL, published $FW_PUBLISHED"

  # 1. the tools: pyOCD and the device pack, both pinned
  setup_pyocd "$home/pyocd-$PYOCD_VERSION"
  fetch_pack "$home/packs"

  # 2. the image, checked against the sha256 the registry computed at publish
  local tmp
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  local hex="$tmp/$HEX_NAME"
  say "downloading $HEX_URL"
  curl -fsSL --retry 2 -o "$hex" "$HEX_URL" </dev/null || die "download failed"
  local got
  got=$(sha256_of "$hex")
  [ "$got" = "$HEX_SHA256" ] || die "sha256 mismatch -- NOT flashing.
   expected $HEX_SHA256
   got      $got"
  say "sha256 OK ($HEX_SIZE bytes)"

  # pyOCD looks for a pyocd.yaml in its working directory; --no-config and an
  # empty cwd make sure nothing on this machine changes what it does.
  cd "$tmp"
  local pyocd=("$PYOCD")
  [ -n "$SUDO" ] && pyocd=("$SUDO" "$PYOCD")
  local common=(--no-config -t "$PYOCD_TARGET" --pack "$PACK")

  # 3. the probe. One cable to J17 (the MCU-Link USB-C) powers the board AND
  # carries SWD; the MCU's own USB-C neither powers it nor can flash it.
  [ -n "$SUDO" ] && say "looking for the board's probe (sudo: the MCU-Link needs root on Linux)"
  # `pyocd json` rather than parsing the `pyocd list` table, whose columns
  # change shape when pyOCD recognises the board.
  local listing ids n
  listing=$("${pyocd[@]}" json --no-config --probes 2>/dev/null </dev/null \
    | "$PYOCD_PY" -c 'import json, sys
for b in json.load(sys.stdin).get("boards", []):
    print("%s\t%s" % (b.get("unique_id", "?"), b.get("info") or b.get("product_name", "")))' 2>/dev/null) || true
  ids=$(printf '%s\n' "$listing" | cut -f1 | grep . || true)
  n=$(printf '%s' "$ids" | grep -c . || true)
  if [ -n "$listing" ]; then
    printf '%s\n' "$listing" | while IFS="$(printf '\t')" read -r uid what; do say "  found $uid  $what"; done
  fi
  if [ -n "$probe" ]; then
    printf '%s\n' "$ids" | grep -qx -- "$probe" || die "no probe with unique ID $probe is connected"
  elif [ "$n" = 0 ]; then
    die "no debug probe found. Check:
   - the USB cable is in J17, the MCU-Link USB-C (it also powers the board),
     and is a DATA cable;
   - jumper J19 is OFF -- with it fitted the onboard MCU-Link cannot drive SWD,
     but its serial console still works, so the board looks half-alive."
  elif [ "$n" -gt 1 ]; then
    die "more than one probe is connected; re-run with  -s -- --probe <Unique ID>"
  else
    probe="$ids"
  fi
  say "probe   $probe"
  common+=(-u "$probe")

  local cmd=("${pyocd[@]}" flash "${common[@]}" "$hex")
  if [ "$dry" = 1 ]; then
    say "dry run -- would run: ${cmd[*]}"
    exit 0
  fi

  # 4. flash. pyOCD erases only the sectors the image covers and resets the
  # board when it is done; the storage partition (config) is in external
  # flash and is not touched.
  echo >&2
  say "flashing MCUboot + the extio app (about 30 s; do not unplug the board)"
  "${cmd[@]}" </dev/null || die "pyOCD failed to flash the board (exit $?)"

  # 5. did it come back as an extio box? That needs the MCU's OWN USB-C plugged
  # in too, so not seeing it is not a failure.
  say "flashed; the board has been reset. Looking for \"extio USB box\"..."
  local i
  for i in $(seq 1 15); do
    if extio_box_present; then
      echo >&2
      say "Done: the $BOARD_TITLE is running extio firmware${FW_IMGVER:+ $FW_IMGVER}."
      next_steps
      exit 0
    fi
    sleep 1
  done
  echo >&2
  say "Flashed. It did not appear as \"extio USB box\" -- expected if only J17 is"
  say "connected: the extio USB interface is on the board's OTHER USB-C, the MCU's own."
  next_steps
}

next_steps() {
  say "Next:"
  say "  - Plug the board's own USB-C (or its Ethernet) into the dserv it belongs to,"
  say "    and restart that dserv so it finds the box."
  say "  - Later updates go over the wire, with no cable to J17:"
  say "      dservctl extio \"extio_ota_push_shelf <box> $FW_CHANNEL\""
}

main "$@"
