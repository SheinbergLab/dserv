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
  log "installing $CAMERA_TCL_DEST"
  mkdir -p "$DSERV_INSTALL/local"

  if [[ -f "$DSERV_SRC/local/camera.tcl" ]]; then
    install -m 0644 "$DSERV_SRC/local/camera.tcl" "$CAMERA_TCL_DEST"
    return
  fi

  cat >"$CAMERA_TCL_DEST" <<'EOF'
#
# Local camera commands (installed by scripts/setup-camera.sh)
#

proc my_frame_handler {frame_id timestamp_ms width height ppm_size ae_settled datapoint_prefix} {
    set timestamp_sec [expr $timestamp_ms / 1000]
    set datetime [clock format $timestamp_sec -format "%Y-%m-%d %H:%M:%S"]
    set info "Frame $frame_id at $datetime: ${width}x${height}, PPM size: $ppm_size bytes, AE settled: $ae_settled"

    dservSet $datapoint_prefix/frame_info $info
    cameraPublishPreviewFrame $frame_id $datapoint_prefix/preview
}

proc start { { camera_id 0 } } {
    if {[catch {cameraInit $camera_id} result]} {
        puts "Error initializing camera: $result"
        return
    }

    # 90° CW — software on Pi (libcamera hardware supports 0/180 only)
    cameraSetRotation 90

    if {[catch {cameraConfigure 1920 1080 30.0 320 240} result]} {
        puts "Error configuring camera: $result"
        return
    }

    if {[catch {cameraStartStreaming} result]} {
        puts "Error starting streaming: $result"
        return
    }

    # ~30fps stream; skip 100 => ~3.3 processed frames/s; every 3rd => ~10s
    cameraSetFrameSkipRate 100
    if {[catch {cameraStartContinuousCallback my_frame_handler "camera" 3} result]} {
        puts "Error starting continuous callback: $result"
        return
    }

    dservSet camera/status continuous
    puts "Camera started - snapshot every ~10s to camera/preview"
}

proc stop {} {
    dservSet camera/status stopped
    catch { cameraStopContinuous }
    catch { cameraStopStreaming }
    puts "Camera stopped"
}

proc check_status {} {
    if {[catch {cameraStatus} status]} {
        puts "Error getting status: $status"
        return
    }
    return $status
}

proc check_ring_buffer {} {
    if {[catch {cameraGetRingBufferStatus} ring_status]} {
        puts "Error getting ring buffer status: $ring_status"
        return
    }
    return $ring_status
}

proc get_frame_ppm {frame_id} {
    if {[catch {cameraGetPpmCallbackFrame $frame_id} ppm_data]} {
        puts "Error getting PPM frame $frame_id: $ppm_data"
        return ""
    }
    return $ppm_data
}

proc get_frame_jpeg {frame_id} {
    if {[catch {cameraGetJpegCallbackFrame $frame_id} jpeg_data]} {
        puts "Error getting JPEG frame $frame_id: $jpeg_data"
        return ""
    }
    return $jpeg_data
}

proc save_frame {frame_id filename} {
    set ext [string tolower [file extension $filename]]
    if {$ext eq ".ppm"} {
        if {[catch {cameraSavePpmCallbackFrame $frame_id $filename} result]} {
            puts "Error saving PPM frame: $result"
            return 0
        }
    } elseif {$ext eq ".jpg" || $ext eq ".jpeg"} {
        if {[catch {cameraSaveJpegCallbackFrame $frame_id $filename} result]} {
            puts "Error saving JPEG frame: $result"
            return 0
        }
    } else {
        puts "Unsupported file extension: $ext (use .ppm, .jpg, or .jpeg)"
        return 0
    }
    puts "Saved frame $frame_id to $filename"
    return 1
}

proc publish_frame {frame_id datapoint_name {format "ppm"}} {
    if {$format eq "ppm"} {
        if {[catch {cameraPublishPpmCallbackFrame $frame_id $datapoint_name} result]} {
            puts "Error publishing PPM frame: $result"
            return 0
        }
    } elseif {$format eq "jpeg"} {
        if {[catch {cameraPublishJpegCallbackFrame $frame_id $datapoint_name} result]} {
            puts "Error publishing JPEG frame: $result"
            return 0
        }
    } else {
        puts "Unsupported format: $format (use 'ppm' or 'jpeg')"
        return 0
    }
    puts "Published frame $frame_id as $format to $datapoint_name"
    return 1
}
EOF
  chmod 0644 "$CAMERA_TCL_DEST"
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
  install_module
  restart_dserv
  verify_camera_subprocess

  log "done"
  echo
  echo "Start capture from the dserv Web Console or Tcl:"
  echo "  send camera start"
  echo
  echo "After starting, verify with:"
  echo "  send camera check_status   ;# expect rotation 90, rotation_mode software"
}

main "$@"
