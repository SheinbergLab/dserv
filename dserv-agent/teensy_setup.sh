# teensy_setup.sh -- flash extio firmware onto a Teensy 4.x (body of /extio/flash for the teensy boards).
#
# Served behind a preamble written by extio_flash.go that sets, for the image
# the registry picked: BOARD_NAME BOARD_TITLE TEENSY_MCU FW_SHELF_URL
# FW_CHANNEL FW_VERSION FW_IMGVER FW_PUBLISHED HEX_URL HEX_NAME HEX_SHA256
# HEX_SIZE. Run it as the preamble's caller does:
#
#   curl -sSL "https://dserv.net/extio/flash?board=teensy40" | bash
#   curl -sSL "https://dserv.net/extio/flash?board=teensy40" | bash -s -- --dry-run
#
# Everything is inside main(), called on the last line, so bash has read the
# whole script before anything runs. Under curl|bash the script IS stdin, and a
# child that reads stdin (brew, apt-get) would otherwise swallow the rest of it.

set -euo pipefail

say()  { printf '>> %s\n' "$*" >&2; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() {
  cat >&2 <<EOF
Flash extio firmware onto a ${BOARD_TITLE:-Teensy 4.x} over USB.

  curl -sSL "${FW_SHELF_URL:-https://dserv.net}/extio/flash?board=${BOARD_NAME:-teensy40}" | bash [-s -- OPTIONS]

URL parameters choose the image:  board=teensy40|teensy41  channel=dev  version=<v>

Options:
  --dry-run      download and check the image, show the flash command, flash nothing
  --no-install   do not install teensy_loader_cli if it is missing; just stop
  -h, --help     this text

This is the FIRST flash: it writes MCUboot and the app together, so it is what
a blank Teensy needs. A Teensy already running extio firmware does not need it
to update -- its dserv can update it over the wire:
  dservctl extio "extio_ota_push_shelf <box> ${FW_CHANNEL:-dev}"
EOF
}

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else die "need shasum or sha256sum to check the download"; fi
}

# The firmware's USB product string (extio-zephyr/src/platform/box_usbd.c) --
# the same name dserv's extio USB module looks for.
extio_box_present() {
  case "$(uname -s)" in
    Darwin) ioreg -p IOUSB -w0 2>/dev/null | grep -q 'extio USB box' ;;
    Linux)  grep -qx 'extio USB box' /sys/bus/usb/devices/*/product 2>/dev/null ;;
    *)      return 1 ;;
  esac
}

install_loader() {
  case "$(uname -s)" in
    Darwin)
      command -v brew >/dev/null 2>&1 || die "teensy_loader_cli is not installed and Homebrew is not available.
   Install Homebrew (https://brew.sh) and re-run, or flash by hand with PJRC's
   Teensy Loader app: download $HEX_URL
   and open it in the app (File > Open HEX File), then press the Program button."
      say "installing teensy_loader_cli (brew install teensy_loader_cli)"
      brew install teensy_loader_cli </dev/null
      ;;
    Linux)
      command -v apt-get >/dev/null 2>&1 || die "teensy_loader_cli is not installed; install it with your package manager
   (Debian/Ubuntu: teensy-loader-cli) and re-run."
      say "installing teensy_loader_cli (apt-get install teensy-loader-cli; asks for your password)"
      $SUDO apt-get install -y teensy-loader-cli </dev/null
      ;;
    *)
      die "unsupported system $(uname -s). Download $HEX_URL
   and flash it with PJRC's Teensy Loader app (File > Open HEX File)."
      ;;
  esac
  command -v teensy_loader_cli >/dev/null 2>&1 || die "teensy_loader_cli still not on PATH after installing it"
}

main() {
  local dry=0 noinstall=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --dry-run)    dry=1 ;;
      --no-install) noinstall=1 ;;
      -h|--help)    usage; exit 0 ;;
      *)            usage; die "unknown option: $1" ;;
    esac
    shift
  done

  : "${HEX_URL:?no image URL -- run this through ${FW_SHELF_URL:-https://dserv.net}/extio/flash?board=...}"

  SUDO=""
  if [ "$(uname -s)" = Linux ] && [ "$(id -u)" != 0 ]; then
    command -v sudo >/dev/null 2>&1 && SUDO=sudo
  fi

  say "extio firmware for a $BOARD_TITLE"
  say "  image   $HEX_NAME  ($FW_CHANNEL $FW_VERSION${FW_IMGVER:+, firmware $FW_IMGVER})"
  say "  from    $FW_SHELF_URL, published $FW_PUBLISHED"

  # 1. the loader
  if ! command -v teensy_loader_cli >/dev/null 2>&1; then
    if [ "$noinstall" = 1 ] || [ "$dry" = 1 ]; then
      die "teensy_loader_cli is not installed (macOS: brew install teensy_loader_cli; Debian/Ubuntu: apt install teensy-loader-cli)"
    fi
    install_loader
  fi
  say "loader  $(command -v teensy_loader_cli)"

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

  local cmd=(teensy_loader_cli "--mcu=$TEENSY_MCU" -w -s -v "$hex")
  [ -n "$SUDO" ] && cmd=("$SUDO" "${cmd[@]}")

  if [ "$dry" = 1 ]; then
    say "dry run -- would run: ${cmd[*]}"
    exit 0
  fi

  # 3. flash
  echo >&2
  if extio_box_present; then
    say "An extio box is already plugged in. The loader cannot reboot extio firmware"
    say "into the bootloader by itself, so the button is needed."
    say "(If dserv on this machine has it open, that is fine: flashing does not use its port.)"
  fi
  say "Make sure the board is a $BOARD_TITLE, plugged into this computer with a DATA cable."
  say "When the loader says it is waiting, press the Program button on the Teensy"
  say "(the small white button). A brand-new Teensy usually starts on its own."
  echo >&2
  "${cmd[@]}" </dev/null || die "teensy_loader_cli failed (exit $?)"

  # 4. did it come back as an extio box?
  say "flashed; waiting for the board to come back as \"extio USB box\"..."
  local i
  for i in $(seq 1 20); do
    if extio_box_present; then
      echo >&2
      say "Done: the $BOARD_TITLE is running extio firmware${FW_IMGVER:+ $FW_IMGVER}."
      say "Next:"
      say "  - Restart dserv on the computer it is plugged into, so dserv finds the box."
      say "  - Later updates go over the wire, with no button press:"
      say "      dservctl extio \"extio_ota_push_shelf <box> $FW_CHANNEL\""
      exit 0
    fi
    sleep 1
  done
  warn "the board did not show up as \"extio USB box\" within 20 s."
  warn "Check the cable and port, then unplug and replug the board. If it still does"
  warn "not appear, re-run this and press the Program button when asked."
  exit 1
}

main "$@"
