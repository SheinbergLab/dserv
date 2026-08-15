#!/usr/bin/env bash
# Setup dserv camera module and local config. Does NOT run "send camera start".
#
# Installs build deps (if missing), builds dserv_camera.so from repo sources,
# installs the module + local/camera.tcl, restarts dserv, and verifies the
# camera subprocess is registered.
#
# Usage:
#   sudo ./scripts/setup-camera.sh
#   DSERV_SRC=/path/to/dserv DSERV_INSTALL=/usr/local/dserv sudo ./scripts/setup-camera.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DSERV_SRC="${DSERV_SRC:-$(cd "$SCRIPT_DIR/.." && pwd)}"
DSERV_INSTALL="${DSERV_INSTALL:-/usr/local/dserv}"
BUILD_DIR="${BUILD_DIR:-/tmp/dserv-camera-build}"
MODULE_DEST="$DSERV_INSTALL/modules/dserv_camera.so"
CAMERA_TCL_DEST="$DSERV_INSTALL/local/camera.tcl"
DSERV_PORT="${DSERV_PORT:-2560}"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [[ "$(id -u)" -ne 0 ]]; then
  die "run as root (sudo $0)"
fi

[[ -f "$DSERV_SRC/modules/camera/camera.cpp" ]] || die "missing source: $DSERV_SRC/modules/camera/camera.cpp"

install_build_deps() {
  local missing=()
  command -v cmake >/dev/null || missing+=(cmake)
  command -v g++ >/dev/null || missing+=(g++)
  pkg-config --exists libcamera 2>/dev/null || missing+=(libcamera-dev)
  [[ -f /usr/include/tcl9.0/tcl.h ]] || missing+=(tcl9.0-dev)
  pkg-config --exists libjpeg 2>/dev/null || missing+=(libjpeg-dev)

  if ((${#missing[@]})); then
    log "installing build dependencies: ${missing[*]}"
    apt-get update -qq
    apt-get install -y --no-install-recommends "${missing[@]}"
  else
    log "build dependencies already present"
  fi
}

build_module() {
  log "building dserv_camera.so from $DSERV_SRC/modules/camera/camera.cpp"
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"

  cat >"$BUILD_DIR/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.15)
project(camera_capture)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Release)

add_definitions(-DUSE_TCL_STUBS -DLINUX -DHAS_LIBCAMERA -DHAS_JPEG -fPIC)
include_directories("$DSERV_SRC/src" /usr/include/tcl9.0 /usr/local/include)
link_directories(/usr/local/lib /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
find_package(JPEG REQUIRED)

add_library(camera_capture MODULE "$DSERV_SRC/modules/camera/camera.cpp")
set_target_properties(camera_capture PROPERTIES PREFIX "dserv_" OUTPUT_NAME "camera")
target_include_directories(camera_capture PRIVATE \${LIBCAMERA_INCLUDE_DIRS} \${JPEG_INCLUDE_DIRS})
target_link_libraries(camera_capture tclstub9.0 \${LIBCAMERA_LIBRARIES} \${JPEG_LIBRARIES} pthread)
EOF

  cmake -S "$BUILD_DIR" -B "$BUILD_DIR/build"
  cmake --build "$BUILD_DIR/build" --target camera_capture -j"$(nproc 2>/dev/null || echo 2)"

  [[ -f "$BUILD_DIR/build/dserv_camera.so" ]] || die "build failed: no dserv_camera.so"
  if ldd "$BUILD_DIR/build/dserv_camera.so" | grep -q 'not found'; then
    die "built module has unresolved libraries — check libcamera install"
  fi
}

install_camera_tcl() {
  mkdir -p "$DSERV_INSTALL/local"

  # Prefer a rig-tuned config from the source clone; otherwise keep whatever
  # the rig already has (rotation/exposure tuning lives there); otherwise
  # start from the repo EXAMPLE (~1 snapshot/sec).
  if [[ -f "$DSERV_SRC/local/camera.tcl" ]]; then
    log "installing $CAMERA_TCL_DEST (from $DSERV_SRC/local/camera.tcl)"
    install -m 0644 "$DSERV_SRC/local/camera.tcl" "$CAMERA_TCL_DEST"
  elif [[ -f "$CAMERA_TCL_DEST" ]]; then
    log "keeping existing $CAMERA_TCL_DEST"
  else
    log "installing $CAMERA_TCL_DEST (from repo EXAMPLE)"
    install -m 0644 "$DSERV_SRC/local/camera.tcl.EXAMPLE" "$CAMERA_TCL_DEST"
  fi
}

install_www() {
  # The feed page ships with dserv installs; copy it here too so a rig whose
  # installed dserv predates the page still gets it.
  if [[ -d "$DSERV_INSTALL/www" && -f "$DSERV_SRC/www/camera.html" ]]; then
    log "installing $DSERV_INSTALL/www/camera.html"
    install -m 0644 "$DSERV_SRC/www/camera.html" "$DSERV_INSTALL/www/camera.html"
  fi
}

install_module() {
  log "installing $MODULE_DEST"
  mkdir -p "$DSERV_INSTALL/modules"
  install -m 0755 "$BUILD_DIR/build/dserv_camera.so" "$MODULE_DEST"
}

restart_dserv() {
  log "restarting dserv"
  if systemctl is-enabled dserv >/dev/null 2>&1; then
    systemctl restart dserv
  elif [[ -x "$DSERV_INSTALL/dserv" ]]; then
    die "dserv systemd unit not found — restart dserv manually"
  else
    die "dserv not found at $DSERV_INSTALL"
  fi

  for _ in $(seq 1 30); do
    if systemctl is-active dserv >/dev/null 2>&1; then
      sleep 2
      return 0
    fi
    sleep 1
  done
  die "dserv did not become active after restart"
}

verify_camera_subprocess() {
  log "checking camera subprocess is registered"
  python3 <<PY
import socket, struct, sys, time

host = "127.0.0.1"
port = int("${DSERV_PORT}")

for attempt in range(15):
    try:
        s = socket.create_connection((host, port), timeout=2)
        cmd = b"dservGet dserv/interps"
        s.sendall(struct.pack(">I", len(cmd)) + cmd)
        rlen = struct.unpack(">I", s.recv(4))[0]
        interps = s.recv(rlen).decode().split()
        if "camera" in interps:
            print("camera subprocess registered:", " ".join(interps))
            sys.exit(0)
    except OSError:
        pass
    time.sleep(1)

print("camera subprocess NOT in dserv/interps", file=sys.stderr)
sys.exit(1)
PY
}

main() {
  log "DSERV_SRC=$DSERV_SRC"
  log "DSERV_INSTALL=$DSERV_INSTALL"

  install_build_deps
  build_module
  install_camera_tcl
  install_www
  install_module
  restart_dserv
  verify_camera_subprocess

  log "done"
  echo
  echo "Start ~1 snapshot/sec from the dserv Web Console or Tcl:"
  echo "  send camera start"
  echo
  echo "Watch the feed (not linked from the landing page):"
  echo "  https://$(hostname -s).local:2565/camera.html"
  echo
  echo "Check camera state (rotation, exposure, fps) with:"
  echo "  send camera check_status"
}

main "$@"
