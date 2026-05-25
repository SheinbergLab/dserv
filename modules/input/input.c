/*
 * NAME
 *   input.c - Unified kernel input device module
 *
 * DESCRIPTION
 *   Reads events from kernel input devices (libevdev on Linux, stub on
 *   macOS) and publishes per-device-class datapoints. Replaces dserv_touch
 *   with a multi-class skeleton: Phase 1 implements the touchscreen class
 *   byte-compatible with touch.c; later phases add trackpad etc.
 *
 *   Consumers of the existing mtouch/event datapoint are unaffected.
 *
 * AUTHOR
 *   DLS, input-layer reorg
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dirent.h>
#include <limits.h>

#ifdef __linux__
#include <libevdev/libevdev.h>
#include <linux/input.h>
#endif

#ifdef __APPLE__
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdint.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define INPUT_MAX_CLASSES          8
#define INPUT_MAX_EXPECTS          8
#define INPUT_MAX_KNOWN_DEVICES    32
#define INPUT_PATH_MAX             256
#define INPUT_DPOINT_MAX           64
#define INPUT_PATTERN_MAX          128

struct input_state_s;
struct input_device_s;

typedef void *(*input_reader_fn)(void *arg);

#ifdef __linux__
typedef int (*input_match_fn)(struct libevdev *dev);
#else
typedef int (*input_match_fn)(void *dev);
#endif

typedef struct input_class_s {
  char name[32];
  char datapoint[INPUT_DPOINT_MAX];
  input_match_fn matches;
  input_reader_fn reader;

  /* Class-level config defaults applied when a device is opened. */
  int screen_width;
  int screen_height;
  int rotation;               /* -1 = auto-detect from HDMI */
  int track_drag;
  char hdmi_output[32];
} input_class_t;

typedef struct input_device_s {
  struct input_state_s *state;
  input_class_t *cls;
  pthread_t thread_id;
  int thread_running;

  int fd;
  char path[INPUT_PATH_MAX];
  char point_name[INPUT_DPOINT_MAX];

#ifdef __linux__
  struct libevdev *dev;
#endif

#ifdef __APPLE__
  /* macOS IOKit HID state. The reader thread runs a CFRunLoop and
     parses raw input reports from a WPTP-class trackpad. */
  IOHIDDeviceRef hid_device;
  CFRunLoopRef   run_loop;          /* set by reader; CFRunLoopStop wakes it */
  uint8_t        report_buf[256];   /* IOKit fills this for each input report */
  /* Slot 0 state for press/drag/release detection (mirrors the Linux
     trackpad_reader; v1 tracks the primary contact only). */
  int            slot0_tip;
  int            slot0_x;
  int            slot0_y;
  int            slot0_first_after_press;
#endif

  /* Per-device touchscreen state. Lives here (not in the class) so each
     device gets its own axis ranges. */
  int minx, maxx, miny, maxy;
  float rangex, rangey;
  int screen_width, screen_height, rotation;
  int track_drag;

  struct input_device_s *next;
} input_device_t;

typedef struct input_expect_s {
  char class_name[32];
  int required;               /* 1 = fail startup if missing, 0 = optional */
} input_expect_t;

/* Sentinels in known_device_t: -1 means "unset" for dims/track_drag;
   rotation uses -2 = unset, -1 = auto-detect from HDMI, >=0 = explicit. */
typedef struct known_device_s {
  char class_name[32];
  char pattern[INPUT_PATTERN_MAX];
  int screen_w;
  int screen_h;
  int rotation;
  int track_drag;
  char hdmi_output[32];
} known_device_t;

typedef struct input_state_s {
  tclserver_t *tclserver;

  input_class_t classes[INPUT_MAX_CLASSES];
  int n_classes;

  input_device_t *devices;
  pthread_mutex_t devices_lock;

  input_expect_t expects[INPUT_MAX_EXPECTS];
  int n_expects;

  known_device_t known[INPUT_MAX_KNOWN_DEVICES];
  int n_known;
} input_state_t;

static input_class_t *find_class(input_state_t *st, const char *name)
{
  for (int i = 0; i < st->n_classes; i++) {
    if (strcmp(st->classes[i].name, name) == 0) return &st->classes[i];
  }
  return NULL;
}

static int class_has_open_device(input_state_t *st, input_class_t *cls)
{
  pthread_mutex_lock(&st->devices_lock);
  for (input_device_t *d = st->devices; d; d = d->next) {
    if (d->cls == cls) {
      pthread_mutex_unlock(&st->devices_lock);
      return 1;
    }
  }
  pthread_mutex_unlock(&st->devices_lock);
  return 0;
}

/* Last-match-wins so local/input.tcl entries override built-in defaults
   that inputconf.tcl seeded earlier. */
static known_device_t *find_known_device(input_state_t *st,
                                         input_class_t *cls,
                                         const char *by_id_name,
                                         const char *libevdev_name)
{
  for (int i = st->n_known - 1; i >= 0; i--) {
    known_device_t *k = &st->known[i];
    if (strcmp(k->class_name, cls->name) != 0) continue;
    if (by_id_name && *by_id_name &&
        Tcl_StringMatch(by_id_name, k->pattern)) return k;
    if (libevdev_name && *libevdev_name &&
        Tcl_StringMatch(libevdev_name, k->pattern)) return k;
  }
  return NULL;
}

#ifdef __linux__
/* Reverse-lookup the /dev/input/by-id/* symlink pointing to event_path. */
static int find_by_id_name(const char *event_path,
                           char *out, size_t out_size)
{
  DIR *dir = opendir("/dev/input/by-id");
  if (!dir) return 0;

  char resolved_event[PATH_MAX];
  if (!realpath(event_path, resolved_event)) {
    closedir(dir);
    return 0;
  }

  struct dirent *ent;
  int found = 0;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    if (!strstr(ent->d_name, "-event")) continue;

    char sym[PATH_MAX];
    snprintf(sym, sizeof(sym), "/dev/input/by-id/%s", ent->d_name);

    char resolved_link[PATH_MAX];
    if (!realpath(sym, resolved_link)) continue;

    if (strcmp(resolved_link, resolved_event) == 0) {
      strncpy(out, ent->d_name, out_size - 1);
      out[out_size - 1] = '\0';
      found = 1;
      break;
    }
  }
  closedir(dir);
  return found;
}
#endif

#ifdef __linux__

/*****************************************************************************
 * Linux: HDMI rotation discovery (preserved from touch.c)
 *****************************************************************************/

static int get_hdmi_rotation(const char *output_name)
{
  FILE *f = fopen("/proc/cmdline", "r");
  if (!f) return 0;

  char buf[4096];
  if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
  fclose(f);

  char prefix[64];
  snprintf(prefix, sizeof(prefix), "video=%s:", output_name);

  char *p = strstr(buf, prefix);
  if (!p) return 0;

  char *rot = strstr(p, "rotate=");
  if (!rot) return 0;

  char *space = strchr(p, ' ');
  if (space && rot > space) return 0;

  return atoi(rot + 7);
}

/*****************************************************************************
 * Touchscreen class (lifted from touch.c)
 *****************************************************************************/

static int touchscreen_matches(struct libevdev *dev)
{
  if (!libevdev_has_property(dev, INPUT_PROP_DIRECT)) return 0;
  if (!libevdev_has_event_code(dev, EV_ABS, ABS_X)) return 0;
  if (!libevdev_has_event_code(dev, EV_ABS, ABS_Y)) return 0;
  if (!libevdev_has_event_code(dev, EV_KEY, BTN_TOUCH)) return 0;
  return 1;
}

static void *touchscreen_reader(void *arg)
{
  input_device_t *d = (input_device_t *) arg;

  /* Outer loop: each iteration is one "incarnation" of the connected
     device. We re-enter on a successful reconnect after disconnect.
     Per-incarnation state is declared inside the loop so it resets
     cleanly on each reconnect. */
  for (;;) {
    struct input_event ev;
    int raw_x = 0, raw_y = 0;
    int x = 0, y = 0;
    int touch_active = 0;
    int touch_changed = 0;
    int coords_changed = 0;
    int first_coordinate_after_press = 0;
    int rc;

    do {
    rc = libevdev_next_event(d->dev, LIBEVDEV_READ_FLAG_BLOCKING, &ev);
    if (rc != LIBEVDEV_READ_STATUS_SUCCESS) continue;

    switch (ev.type) {
    case EV_KEY:
      if (ev.code == BTN_TOUCH) {
        if (ev.value == 1) {
          touch_active = 1;
          touch_changed = 1;
          first_coordinate_after_press = 0;
        } else if (ev.value == 0) {
          touch_active = 0;
          touch_changed = 1;
        }
      }
      break;

    case EV_ABS:
      if (ev.code == ABS_X && ev.value > 0) {
        raw_x = (int)(d->screen_width *
                      ((ev.value - d->minx) / d->rangex));
        coords_changed = 1;
      } else if (ev.code == ABS_Y && ev.value > 0) {
        raw_y = (int)(d->screen_height *
                      ((ev.value - d->miny) / d->rangey));
        coords_changed = 1;
      }
      break;

    case EV_SYN:
      if (ev.code != SYN_REPORT) break;

      if (coords_changed) {
        switch (d->rotation) {
        case 90:
          x = raw_y;
          y = d->screen_width - 1 - raw_x;
          break;
        case 180:
          x = d->screen_width - 1 - raw_x;
          y = d->screen_height - 1 - raw_y;
          break;
        case 270:
          x = d->screen_height - 1 - raw_y;
          y = raw_x;
          break;
        default:
          x = raw_x;
          y = raw_y;
          break;
        }
      }

      if (touch_active && (coords_changed || touch_changed)) {
        uint16_t vals[3];
        vals[0] = x;
        vals[1] = y;

        if (!first_coordinate_after_press) {
          vals[2] = 0;  /* PRESS */
          first_coordinate_after_press = 1;
        } else if (d->track_drag && coords_changed) {
          vals[2] = 1;  /* DRAG */
        } else {
          goto sync_done;
        }

        ds_datapoint_t *dp = dpoint_new(d->point_name,
                                        tclserver_now(d->state->tclserver),
                                        DSERV_SHORT,
                                        sizeof(vals),
                                        (unsigned char *) vals);
        tclserver_set_point(d->state->tclserver, dp);
      } else if (!touch_active && touch_changed) {
        uint16_t vals[3];
        vals[0] = x;
        vals[1] = y;
        vals[2] = 2;  /* RELEASE */
        ds_datapoint_t *dp = dpoint_new(d->point_name,
                                        tclserver_now(d->state->tclserver),
                                        DSERV_SHORT,
                                        sizeof(vals),
                                        (unsigned char *) vals);
        tclserver_set_point(d->state->tclserver, dp);
      }

    sync_done:
      touch_changed = 0;
      coords_changed = 0;
      break;
    }
  } while (rc >= 0);

    /* Read loop exited — libevdev_next_event returned a negative
       errno. Mirror trackpad_reader's reconnect flow: log, tear down,
       wait via device_reconnect for a replacement, resume. */
    if (rc < 0) {
      fprintf(stderr,
              "input: touchscreen_reader: read loop exited: %s (errno=%d,"
              " path=%s, point=%s); attempting reconnect\n",
              strerror(-rc), -rc, d->path, d->point_name);
      fflush(stderr);
    }

    if (d->dev) { libevdev_free(d->dev); d->dev = NULL; }
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }

    if (device_reconnect(d) < 0) {
      fprintf(stderr,
              "input: touchscreen_reader: reconnect timed out; giving"
              " up. Restart dserv after replugging.\n");
      fflush(stderr);
      return NULL;
    }
    /* Reconnect succeeded — outer for-loop reiterates: resets state,
       re-enters read loop. (Touchscreens don't publish a per-device
       range datapoint the way trackpads do.) */
  }
}

/*****************************************************************************
 * Trackpad class
 *
 * Classifier: pointer-class device with multi-touch absolute position.
 *   INPUT_PROP_POINTER  - not display-direct (excludes touchscreens)
 *   ABS_MT_POSITION_X/Y - multitouch-protocol-B absolute coordinates
 *                         (excludes plain mice, which report REL_X/REL_Y)
 *
 * Publishes:
 *   mtouch/trackpad         uint16[3] (x, y, event_type)  per contact update
 *   mtouch/trackpad/range   int32[4]  (0, span_x, 0, span_y) once at open
 *
 * Event semantics mirror touchscreen (0 = PRESS, 1 = DRAG, 2 = RELEASE).
 * Coordinates are **shifted** so the published values are always
 * non-negative, regardless of the device's native coordinate system.
 * The Apple Magic Trackpad reports a signed range centered on (0, 0)
 * (e.g. X: -3678..3934); the Holtek HTX uses [0, max]. We subtract the
 * device's min so the wire values fit cleanly into uint16 and the
 * downstream Tcl normalize math `(raw - 0) * 4095 / span` works the
 * same for both classes. Range is published as (0, span_x, 0, span_y)
 * to match.
 * Single-contact tracking: we adopt the first slot a contact lands
 * on (any slot, not just slot 0 — the Apple Magic Trackpad assigns
 * slots from a pool, so the first contact can come in on slot 2 or
 * higher) and follow it through DRAG until its tracking_id goes -1.
 * Additional concurrent contacts (multi-finger) are ignored while a
 * contact is tracked. Sufficient for the swipe paradigm; revisit if
 * a paradigm ever needs multi-finger semantics.
 *****************************************************************************/

static int trackpad_matches(struct libevdev *dev)
{
  if (libevdev_has_property(dev, INPUT_PROP_DIRECT)) return 0;
  if (!libevdev_has_property(dev, INPUT_PROP_POINTER)) return 0;
  if (!libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X)) return 0;
  if (!libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y)) return 0;
  return 1;
}

static void publish_trackpad_range(input_device_t *d)
{
  char range_name[INPUT_DPOINT_MAX];
  snprintf(range_name, sizeof(range_name), "%s/range", d->point_name);

  /* Publish the shifted range so it matches the shifted per-event
     coords (see header comment). Min is always 0; max is the span. */
  int32_t vals[4];
  vals[0] = 0;
  vals[1] = d->maxx - d->minx;
  vals[2] = 0;
  vals[3] = d->maxy - d->miny;
  ds_datapoint_t *dp = dpoint_new(range_name,
                                  tclserver_now(d->state->tclserver),
                                  DSERV_INT,
                                  sizeof(vals),
                                  (unsigned char *) vals);
  tclserver_set_point(d->state->tclserver, dp);
}

/* Wait for a /dev/input/event* node matching d's class to appear,
   reopen it, and refresh d's fd/dev/path/range fields in place. Used
   by trackpad_reader and touchscreen_reader on -ENODEV (cable yank /
   hub blip / power glitch / HDMI flicker for resistive touchscreens)
   so a session survives a replug without a dserv restart.

   Class-agnostic except for the axis-range refresh, which dispatches
   on cls->name to mirror what open_device does (ABS_X/ABS_Y for
   touchscreens, ABS_MT_POSITION_X/Y for trackpads). Per-rig overrides
   (screen_width/height/rotation/track_drag for touchscreens) live on
   d already and are intentionally NOT touched here — they describe
   the rig, not the device's firmware, and persist across a replug
   of the same physical device. If someone hot-swaps a touchscreen
   for a different model, the saved overrides will be stale; a full
   dserv restart re-runs autodiscover and reapplies known_device
   overrides from scratch.

   Returns 0 on success, -1 on timeout. nanosleep is a pthread
   cancellation point so stop_device's pthread_cancel still interrupts
   cleanly while we're waiting.

   Race note: while d->dev is NULL, class_has_open_device sees this
   class as "available", so a concurrent inputAutodiscover could in
   principle pick up the new device and create a *second* device record
   for the same path. In practice autodiscover is one-shot at startup
   and not called again during a session, so this isn't exercised.
   If it ever becomes a concern, add a per-device 'recovering' flag
   that class_has_open_device respects. */
static int device_reconnect(input_device_t *d)
{
  const int max_wait_seconds = 120;
  const int poll_interval_ms = 500;
  int elapsed_ms = 0;

  fprintf(stderr,
          "input: %s_reader: waiting up to %ds for a matching device"
          " to reappear at /dev/input\n",
          d->cls->name, max_wait_seconds);
  fflush(stderr);

  while (elapsed_ms < max_wait_seconds * 1000) {
    DIR *dir = opendir("/dev/input");
    if (dir) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[INPUT_PATH_MAX];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        struct libevdev *dev = NULL;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
          close(fd);
          continue;
        }

        if (!d->cls->matches || !d->cls->matches(dev)) {
          libevdev_free(dev);
          close(fd);
          continue;
        }

        /* Found a match. Adopt into d in place — keep the same
           input_device_t struct so the linked list, thread id, and
           point_name all remain valid. */
        d->fd = fd;
        d->dev = dev;
        strncpy(d->path, path, sizeof(d->path) - 1);
        d->path[sizeof(d->path) - 1] = '\0';

        /* Refresh axis ranges. Class-specific — mirrors open_device. */
        if (strcmp(d->cls->name, "touchscreen") == 0) {
          d->minx = libevdev_get_abs_minimum(dev, ABS_X);
          d->maxx = libevdev_get_abs_maximum(dev, ABS_X);
          d->miny = libevdev_get_abs_minimum(dev, ABS_Y);
          d->maxy = libevdev_get_abs_maximum(dev, ABS_Y);
        } else if (strcmp(d->cls->name, "trackpad") == 0) {
          d->minx = libevdev_get_abs_minimum(dev, ABS_MT_POSITION_X);
          d->maxx = libevdev_get_abs_maximum(dev, ABS_MT_POSITION_X);
          d->miny = libevdev_get_abs_minimum(dev, ABS_MT_POSITION_Y);
          d->maxy = libevdev_get_abs_maximum(dev, ABS_MT_POSITION_Y);
        }
        d->rangex = (float)(d->maxx - d->minx);
        d->rangey = (float)(d->maxy - d->miny);

        closedir(dir);
        fprintf(stderr,
                "input: %s_reader: reconnected on %s\n",
                d->cls->name, path);
        fflush(stderr);
        return 0;
      }
      closedir(dir);
    }

    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = (long) poll_interval_ms * 1000000L;
    nanosleep(&ts, NULL);
    elapsed_ms += poll_interval_ms;
  }

  return -1;
}

static void *trackpad_reader(void *arg)
{
  input_device_t *d = (input_device_t *) arg;

  /* Outer loop: each iteration is one "incarnation" of the connected
     device. We re-enter on a successful reconnect after disconnect.
     State (slot tracking, press/drag bookkeeping) is declared inside
     the loop so it resets cleanly on each incarnation. */
  for (;;) {
    struct input_event ev;
    int current_slot = 0;
    /* tracked_slot = the slot we're following for the current contact,
       or -1 if no contact is in progress. We adopt the first slot that
       becomes active (any slot — the Apple Magic Trackpad assigns from
       a pool of 16, so the first finger contact often lands on slot 2+
       after any prior touch history). The HTX always used slot 0; this
       code is a strict superset that handles both. */
    int tracked_slot = -1;
    int contact_active = 0;
    int contact_changed = 0;
    int coords_changed = 0;
    int first_coord_after_press = 0;
    int x = 0, y = 0;
    int rc;

    publish_trackpad_range(d);

    do {
    rc = libevdev_next_event(d->dev, LIBEVDEV_READ_FLAG_BLOCKING, &ev);
    if (rc != LIBEVDEV_READ_STATUS_SUCCESS) continue;

    switch (ev.type) {
    case EV_ABS:
      switch (ev.code) {
      case ABS_MT_SLOT:
        current_slot = ev.value;
        break;

      case ABS_MT_TRACKING_ID:
        if (ev.value >= 0) {
          /* Contact press on current_slot. Adopt as the tracked slot
             only if no contact is currently in progress; additional
             concurrent contacts (multi-finger touches) are ignored. */
          if (tracked_slot == -1) {
            tracked_slot = current_slot;
            contact_active = 1;
            contact_changed = 1;
            first_coord_after_press = 0;
          }
        } else if (current_slot == tracked_slot) {
          /* Release on the tracked slot. Releases on other slots
             (a secondary finger lifting while the primary is still
             down) are ignored. */
          contact_active = 0;
          contact_changed = 1;
          tracked_slot = -1;
        }
        break;

      case ABS_MT_POSITION_X:
        if (current_slot == tracked_slot) { x = ev.value; coords_changed = 1; }
        break;

      case ABS_MT_POSITION_Y:
        if (current_slot == tracked_slot) { y = ev.value; coords_changed = 1; }
        break;
      }
      break;

    case EV_SYN:
      if (ev.code != SYN_REPORT) break;

      if (contact_active && (coords_changed || contact_changed)) {
        uint16_t vals[3];
        /* Shift by device min so signed-range devices (Apple Magic
           Trackpad: X -3678..3934) map to non-negative uint16 cleanly.
           For non-negative-range devices (HTX: minx=0) this is a no-op. */
        vals[0] = (uint16_t) (x - d->minx);
        vals[1] = (uint16_t) (y - d->miny);

        if (!first_coord_after_press) {
          vals[2] = 0;  /* PRESS */
          first_coord_after_press = 1;
        } else if (coords_changed) {
          vals[2] = 1;  /* DRAG */
        } else {
          goto sync_done_tp;
        }

        ds_datapoint_t *dp = dpoint_new(d->point_name,
                                        tclserver_now(d->state->tclserver),
                                        DSERV_SHORT,
                                        sizeof(vals),
                                        (unsigned char *) vals);
        tclserver_set_point(d->state->tclserver, dp);
      } else if (!contact_active && contact_changed) {
        uint16_t vals[3];
        vals[0] = (uint16_t) (x - d->minx);
        vals[1] = (uint16_t) (y - d->miny);
        vals[2] = 2;  /* RELEASE */
        ds_datapoint_t *dp = dpoint_new(d->point_name,
                                        tclserver_now(d->state->tclserver),
                                        DSERV_SHORT,
                                        sizeof(vals),
                                        (unsigned char *) vals);
        tclserver_set_point(d->state->tclserver, dp);
      }

    sync_done_tp:
      contact_changed = 0;
      coords_changed = 0;
      break;
    }
  } while (rc >= 0);

    /* Read loop exited — libevdev_next_event returned a negative
       errno. -ENODEV is the expected path on a USB replug; other
       errnos (EIO etc.) indicate a deeper problem. Log it, tear down
       the current device, then wait via device_reconnect for a
       replacement and resume. If the device never comes back within
       device_reconnect's timeout, the thread exits — at which
       point dserv must be restarted to pick the device up again. */
    if (rc < 0) {
      fprintf(stderr,
              "input: trackpad_reader: read loop exited: %s (errno=%d,"
              " path=%s, point=%s); attempting reconnect\n",
              strerror(-rc), -rc, d->path, d->point_name);
      fflush(stderr);
    }

    if (d->dev) { libevdev_free(d->dev); d->dev = NULL; }
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }

    if (device_reconnect(d) < 0) {
      fprintf(stderr,
              "input: trackpad_reader: reconnect timed out; giving up."
              " Restart dserv after replugging.\n");
      fflush(stderr);
      return NULL;
    }
    /* Reconnect succeeded — outer for-loop reiterates: re-publishes
       trackpad_range, resets per-incarnation state, re-enters read loop. */
  }
}

/*****************************************************************************
 * Device lifecycle (Linux)
 *****************************************************************************/

static input_device_t *open_device(input_state_t *st, input_class_t *cls,
                                   const char *path, Tcl_Interp *interp)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    if (interp) {
      Tcl_AppendResult(interp, "input: cannot open ", path, NULL);
    }
    return NULL;
  }

  struct libevdev *dev = NULL;
  if (libevdev_new_from_fd(fd, &dev) < 0) {
    if (interp) {
      Tcl_AppendResult(interp, "input: libevdev_new_from_fd failed for ",
                       path, NULL);
    }
    close(fd);
    return NULL;
  }

  input_device_t *d = (input_device_t *) calloc(1, sizeof(*d));
  d->state = st;
  d->cls = cls;
  d->fd = fd;
  d->dev = dev;
  strncpy(d->path, path, INPUT_PATH_MAX - 1);
  strncpy(d->point_name, cls->datapoint, INPUT_DPOINT_MAX - 1);

  if (strcmp(cls->name, "touchscreen") == 0) {
    d->minx = libevdev_get_abs_minimum(dev, ABS_X);
    d->maxx = libevdev_get_abs_maximum(dev, ABS_X);
    d->miny = libevdev_get_abs_minimum(dev, ABS_Y);
    d->maxy = libevdev_get_abs_maximum(dev, ABS_Y);
    d->rangex = d->maxx - d->minx;
    d->rangey = d->maxy - d->miny;
    d->screen_width = cls->screen_width;
    d->screen_height = cls->screen_height;
    d->rotation = (cls->rotation >= 0)
                  ? cls->rotation
                  : get_hdmi_rotation(cls->hdmi_output);
    d->track_drag = cls->track_drag;
  } else if (strcmp(cls->name, "trackpad") == 0) {
    d->minx = libevdev_get_abs_minimum(dev, ABS_MT_POSITION_X);
    d->maxx = libevdev_get_abs_maximum(dev, ABS_MT_POSITION_X);
    d->miny = libevdev_get_abs_minimum(dev, ABS_MT_POSITION_Y);
    d->maxy = libevdev_get_abs_maximum(dev, ABS_MT_POSITION_Y);
    d->rangex = d->maxx - d->minx;
    d->rangey = d->maxy - d->miny;
  }

  pthread_mutex_lock(&st->devices_lock);
  d->next = st->devices;
  st->devices = d;
  pthread_mutex_unlock(&st->devices_lock);

  return d;
}

static int start_device(input_device_t *d)
{
  if (d->thread_running) return 0;
  if (pthread_create(&d->thread_id, NULL, d->cls->reader, d) != 0) {
    return -1;
  }
  d->thread_running = 1;
  return 0;
}

static void stop_device(input_device_t *d)
{
  if (d->thread_running) {
    pthread_cancel(d->thread_id);
    pthread_join(d->thread_id, NULL);
    d->thread_running = 0;
  }
  if (d->dev) { libevdev_free(d->dev); d->dev = NULL; }
  if (d->fd >= 0) { close(d->fd); d->fd = -1; }
}

static void destroy_all_devices(input_state_t *st)
{
  pthread_mutex_lock(&st->devices_lock);
  input_device_t *d = st->devices;
  st->devices = NULL;
  pthread_mutex_unlock(&st->devices_lock);

  while (d) {
    input_device_t *next = d->next;
    stop_device(d);
    free(d);
    d = next;
  }
}

/*****************************************************************************
 * Autodiscover (Linux)
 *****************************************************************************/

static void unlink_and_free_device(input_state_t *st, input_device_t *d)
{
  pthread_mutex_lock(&st->devices_lock);
  if (st->devices == d) {
    st->devices = d->next;
  } else {
    for (input_device_t *p = st->devices; p && p->next; p = p->next) {
      if (p->next == d) { p->next = d->next; break; }
    }
  }
  pthread_mutex_unlock(&st->devices_lock);
  stop_device(d);
  free(d);
}

static void apply_known_overrides(input_device_t *d, known_device_t *kd,
                                  input_class_t *cls)
{
  if (!kd) return;
  if (strcmp(cls->name, "touchscreen") == 0) {
    if (kd->screen_w > 0)    d->screen_width  = kd->screen_w;
    if (kd->screen_h > 0)    d->screen_height = kd->screen_h;
    if (kd->track_drag >= 0) d->track_drag    = kd->track_drag;
    if (kd->rotation != -2) {
      if (kd->rotation >= 0) {
        d->rotation = kd->rotation;
      } else {
        const char *hdmi = kd->hdmi_output[0] ? kd->hdmi_output
                                              : cls->hdmi_output;
        d->rotation = get_hdmi_rotation(hdmi);
      }
    }
  }
}

static int autodiscover_impl(input_state_t *st, Tcl_Interp *interp,
                             Tcl_Obj *result_dict)
{
  DIR *dir = opendir("/dev/input");
  if (!dir) return TCL_OK;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;

    char path[INPUT_PATH_MAX];
    snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); continue; }

    input_class_t *matched = NULL;
    for (int i = 0; i < st->n_classes; i++) {
      input_class_t *c = &st->classes[i];
      if (c->matches && c->matches(dev) && !class_has_open_device(st, c)) {
        matched = c;
        break;
      }
    }

    if (!matched) {
      libevdev_free(dev);
      close(fd);
      continue;
    }

    const char *lib_name = libevdev_get_name(dev);
    char by_id[INPUT_PATH_MAX] = "";
    find_by_id_name(path, by_id, sizeof(by_id));

    known_device_t *kd = find_known_device(st, matched, by_id, lib_name);

    /* Touchscreen needs dimensions from the known-device entry or from
       class-level inputConfigure. Without either, skip with a warning —
       inputValidateExpectations will then fail loudly if it was required. */
    if (strcmp(matched->name, "touchscreen") == 0) {
      int sw = (kd && kd->screen_w > 0) ? kd->screen_w : matched->screen_width;
      int sh = (kd && kd->screen_h > 0) ? kd->screen_h : matched->screen_height;
      if (sw <= 0 || sh <= 0) {
        fprintf(stderr,
                "input: touchscreen at %s (%s) has no known dimensions; "
                "skipping. Add inputKnownDevice or inputConfigure to enable.\n",
                path, lib_name ? lib_name : "?");
        libevdev_free(dev);
        close(fd);
        continue;
      }
    }

    libevdev_free(dev);
    close(fd);

    input_device_t *d = open_device(st, matched, path, interp);
    if (!d) continue;

    apply_known_overrides(d, kd, matched);

    if (start_device(d) < 0) {
      unlink_and_free_device(st, d);
      continue;
    }

    if (result_dict) {
      Tcl_Obj *info = Tcl_NewDictObj();
      Tcl_DictObjPut(interp, info,
                     Tcl_NewStringObj("path", -1),
                     Tcl_NewStringObj(path, -1));
      if (*by_id) {
        Tcl_DictObjPut(interp, info,
                       Tcl_NewStringObj("by_id", -1),
                       Tcl_NewStringObj(by_id, -1));
      }
      if (lib_name) {
        Tcl_DictObjPut(interp, info,
                       Tcl_NewStringObj("name", -1),
                       Tcl_NewStringObj(lib_name, -1));
      }
      Tcl_DictObjPut(interp, result_dict,
                     Tcl_NewStringObj(matched->name, -1),
                     info);
    }
  }

  closedir(dir);
  return TCL_OK;
}

#else /* !__linux__ */

/*****************************************************************************
 * macOS implementation (IOKit HID, WPTP touchpads)
 *
 * Strategy:
 *   - autodiscover_impl uses IOHIDManager to enumerate trackpads matching
 *     a known VID/PID table. For each match, it allocates an input_device_t
 *     and starts a reader thread.
 *   - The reader thread opens the device with kIOHIDOptionsTypeSeizeDevice
 *     (otherwise macOS's HID stack silently consumes the reports), writes
 *     two Feature reports to switch the device from default mouse mode
 *     into Windows Precision Touchpad mode (this is what hid-multitouch.ko
 *     does on Linux probe), then runs a CFRunLoop on which IOKit
 *     dispatches raw input reports.
 *   - Each input report (ID 4) is parsed and publishes mtouch/trackpad
 *     events with the same uint16[3] (x, y, event_type) format as the
 *     Linux trackpad_reader. Range is published once at open.
 *
 * Touchscreens are not yet supported on macOS (would need a similar
 * IOKit backend with Digitizer-class matching). Stubs left in place.
 *****************************************************************************/

/* Known WPTP-class trackpads. Extend this table to enable additional
 * devices. Axis ranges come from the device's HID descriptor (which
 * Linux's evtest also reports). */
typedef struct {
  int vid;
  int pid;
  int min_x, max_x;
  int min_y, max_y;
} mac_known_trackpad_t;

static const mac_known_trackpad_t MAC_KNOWN_TRACKPADS[] = {
  /* HTX HID Device — Holtek HTK2288 master + Pixcir PCT1335QN touch IC */
  { 0x048d, 0x8911, 0, 1599, 0, 1199 },
};
#define MAC_KNOWN_TRACKPAD_COUNT \
  (sizeof(MAC_KNOWN_TRACKPADS) / sizeof(MAC_KNOWN_TRACKPADS[0]))

static const mac_known_trackpad_t *find_mac_known_trackpad(int vid, int pid)
{
  for (size_t i = 0; i < MAC_KNOWN_TRACKPAD_COUNT; i++) {
    if (MAC_KNOWN_TRACKPADS[i].vid == vid &&
        MAC_KNOWN_TRACKPADS[i].pid == pid) {
      return &MAC_KNOWN_TRACKPADS[i];
    }
  }
  return NULL;
}

/* Class-table stubs. Real matching on macOS happens in autodiscover_impl
 * via IOHIDManager's matching dictionary — these per-device callbacks
 * are not used. */
static int touchscreen_matches(void *dev) { (void)dev; return 0; }
static void *touchscreen_reader(void *arg) { (void)arg; return NULL; }
static int trackpad_matches(void *dev) { (void)dev; return 0; }

/* ----- WPTP enable + report parsing helpers ------------------------- */

static int enable_wptp_mode(IOHIDDeviceRef dev)
{
  /* Report ID 3 (Input Mode): 0 = legacy mouse, 3 = WPTP */
  uint8_t mode[2] = { 0x03, 0x03 };
  IOReturn r1 = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0x03,
                                     mode, sizeof(mode));
  /* Report ID 5 (Surface Switch | Button Switch): both enabled */
  uint8_t sel[2] = { 0x05, 0x03 };
  IOReturn r2 = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0x05,
                                     sel, sizeof(sel));
  return (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? 0 : -1;
}

static void publish_trackpad_event_mac(input_device_t *d,
                                       int x, int y, int evt)
{
  uint16_t vals[3];
  /* Shift by device min so signed-range devices (Magic Trackpad) map
     to non-negative uint16. No-op for non-negative-range devices. */
  vals[0] = (uint16_t) (x - d->minx);
  vals[1] = (uint16_t) (y - d->miny);
  vals[2] = (uint16_t) evt;
  ds_datapoint_t *dp = dpoint_new(d->point_name,
                                  tclserver_now(d->state->tclserver),
                                  DSERV_SHORT,
                                  sizeof(vals),
                                  (unsigned char *) vals);
  tclserver_set_point(d->state->tclserver, dp);
}

static void publish_trackpad_range_mac(input_device_t *d)
{
  char range_name[INPUT_DPOINT_MAX];
  snprintf(range_name, sizeof(range_name), "%s/range", d->point_name);
  /* Publish the shifted range so it matches the shifted per-event
     coords (see header comment). Min is always 0; max is the span. */
  int32_t vals[4];
  vals[0] = 0;
  vals[1] = d->maxx - d->minx;
  vals[2] = 0;
  vals[3] = d->maxy - d->miny;
  ds_datapoint_t *dp = dpoint_new(range_name,
                                  tclserver_now(d->state->tclserver),
                                  DSERV_INT,
                                  sizeof(vals),
                                  (unsigned char *) vals);
  tclserver_set_point(d->state->tclserver, dp);
}

/* WPTP Report ID 4 layout (see hid_test.c for full breakdown):
 *   byte 0    : report ID (= 4)
 *   bytes 1-5 : finger slot 0 — flags + 16-bit X + 16-bit Y (little-endian)
 *   bytes 6-25: slots 1-4 (same shape)
 *   bytes 26-27: scan time, byte 28: contact count, byte 29: buttons
 * Slot 0 byte 0: bit 0 confidence, bit 1 tip switch, bits 2-4 contact ID. */
static void parse_wptp_report_mac(input_device_t *d, uint8_t *r, CFIndex len)
{
  if (len < 30 || r[0] != 4) return;

  const uint8_t *b = &r[1];
  int tip = (b[0] >> 1) & 0x01;
  int x = b[1] | (b[2] << 8);
  int y = b[3] | (b[4] << 8);

  if (!d->slot0_tip && tip) {
    /* PRESS */
    d->slot0_tip = 1;
    d->slot0_x = x;
    d->slot0_y = y;
    d->slot0_first_after_press = 1;
    publish_trackpad_event_mac(d, x, y, 0);
  } else if (d->slot0_tip && !tip) {
    /* RELEASE — use last known position (device sends final position
     * with tip=0; we keep the previous coords for consistency with the
     * Linux trackpad_reader). */
    d->slot0_tip = 0;
    publish_trackpad_event_mac(d, d->slot0_x, d->slot0_y, 2);
  } else if (tip && (d->slot0_x != x || d->slot0_y != y)) {
    /* DRAG */
    d->slot0_x = x;
    d->slot0_y = y;
    publish_trackpad_event_mac(d, x, y, 1);
  }
}

static void on_hid_report_callback(void *ctx, IOReturn result, void *sender,
                                   IOHIDReportType type, uint32_t reportID,
                                   uint8_t *report, CFIndex reportLength)
{
  (void)sender; (void)type;
  if (result != kIOReturnSuccess) return;
  if (reportID != 4) return;
  parse_wptp_report_mac((input_device_t *) ctx, report, reportLength);
}

/* Reader thread: open device, enable WPTP, publish range, register
 * callback, run CFRunLoop until stop_device tells us to exit. */
static void *trackpad_reader(void *arg)
{
  input_device_t *d = (input_device_t *) arg;
  if (!d->hid_device) return NULL;

  if (IOHIDDeviceOpen(d->hid_device, kIOHIDOptionsTypeSeizeDevice)
      != kIOReturnSuccess) {
    return NULL;
  }
  if (enable_wptp_mode(d->hid_device) < 0) {
    IOHIDDeviceClose(d->hid_device, kIOHIDOptionsTypeNone);
    return NULL;
  }

  publish_trackpad_range_mac(d);

  d->slot0_tip = 0;
  d->slot0_x = 0;
  d->slot0_y = 0;
  d->slot0_first_after_press = 0;

  d->run_loop = CFRunLoopGetCurrent();
  IOHIDDeviceScheduleWithRunLoop(d->hid_device, d->run_loop,
                                 kCFRunLoopDefaultMode);
  IOHIDDeviceRegisterInputReportCallback(d->hid_device,
                                         d->report_buf,
                                         (CFIndex)sizeof(d->report_buf),
                                         on_hid_report_callback, d);

  CFRunLoopRun();  /* blocks until CFRunLoopStop in stop_device */

  IOHIDDeviceUnscheduleFromRunLoop(d->hid_device, d->run_loop,
                                   kCFRunLoopDefaultMode);
  IOHIDDeviceClose(d->hid_device, kIOHIDOptionsTypeNone);
  return NULL;
}

/* ----- Device lifecycle (macOS) -------------------------------- */

/* Construct input_device_t bound to an IOHIDDeviceRef discovered by
 * autodiscover_impl. CFRetain keeps the device alive while we hold it. */
static input_device_t *open_device_from_hid(input_state_t *st,
                                            input_class_t *cls,
                                            IOHIDDeviceRef hid_dev)
{
  int vid = 0, pid = 0;
  CFTypeRef vt = IOHIDDeviceGetProperty(hid_dev, CFSTR(kIOHIDVendorIDKey));
  CFTypeRef pt = IOHIDDeviceGetProperty(hid_dev, CFSTR(kIOHIDProductIDKey));
  if (vt) CFNumberGetValue((CFNumberRef)vt, kCFNumberIntType, &vid);
  if (pt) CFNumberGetValue((CFNumberRef)pt, kCFNumberIntType, &pid);

  const mac_known_trackpad_t *kt = find_mac_known_trackpad(vid, pid);
  if (!kt) return NULL;

  input_device_t *d = (input_device_t *) calloc(1, sizeof(*d));
  d->state = st;
  d->cls = cls;
  d->fd = -1;
  snprintf(d->path, INPUT_PATH_MAX, "iohid:%04x:%04x", vid, pid);
  strncpy(d->point_name, cls->datapoint, INPUT_DPOINT_MAX - 1);
  d->hid_device = hid_dev;
  CFRetain(d->hid_device);

  d->minx = kt->min_x;
  d->maxx = kt->max_x;
  d->miny = kt->min_y;
  d->maxy = kt->max_y;
  d->rangex = (float)(d->maxx - d->minx);
  d->rangey = (float)(d->maxy - d->miny);

  pthread_mutex_lock(&st->devices_lock);
  d->next = st->devices;
  st->devices = d;
  pthread_mutex_unlock(&st->devices_lock);

  return d;
}

/* inputOpen Tcl command — Linux takes a /dev/input/eventN path. On macOS
 * that doesn't apply; force users through inputAutodiscover. */
static input_device_t *open_device(input_state_t *st, input_class_t *cls,
                                   const char *path, Tcl_Interp *interp)
{
  (void)st; (void)cls; (void)path;
  if (interp) {
    Tcl_AppendResult(interp,
      "input: inputOpen by path not supported on macOS — "
      "use inputAutodiscover", NULL);
  }
  return NULL;
}

static int start_device(input_device_t *d)
{
  if (d->thread_running) return 0;
  if (pthread_create(&d->thread_id, NULL, d->cls->reader, d) != 0) return -1;
  d->thread_running = 1;
  return 0;
}

static void stop_device(input_device_t *d)
{
  if (d->thread_running) {
    if (d->run_loop) CFRunLoopStop(d->run_loop);
    pthread_join(d->thread_id, NULL);
    d->thread_running = 0;
    d->run_loop = NULL;
  }
  if (d->hid_device) {
    CFRelease(d->hid_device);
    d->hid_device = NULL;
  }
}

static void unlink_and_free_device(input_state_t *st, input_device_t *d)
{
  pthread_mutex_lock(&st->devices_lock);
  if (st->devices == d) {
    st->devices = d->next;
  } else {
    for (input_device_t *p = st->devices; p && p->next; p = p->next) {
      if (p->next == d) { p->next = d->next; break; }
    }
  }
  pthread_mutex_unlock(&st->devices_lock);
  stop_device(d);
  free(d);
}

static void destroy_all_devices(input_state_t *st)
{
  pthread_mutex_lock(&st->devices_lock);
  input_device_t *d = st->devices;
  st->devices = NULL;
  pthread_mutex_unlock(&st->devices_lock);
  while (d) {
    input_device_t *next = d->next;
    stop_device(d);
    free(d);
    d = next;
  }
}

/* Autodiscover: build a multi-match dictionary from MAC_KNOWN_TRACKPADS
 * (filtered to the Mouse interface — the WPTP touchpad exposes data on
 * UsagePage=1 Usage=2), enumerate currently-attached matches, and start
 * a reader thread for each. */
static int autodiscover_impl(input_state_t *st, Tcl_Interp *interp,
                             Tcl_Obj *result_dict)
{
  input_class_t *cls = find_class(st, "trackpad");
  if (!cls) return TCL_OK;

  CFMutableArrayRef match_arr = CFArrayCreateMutable(
    kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

  int upage_v = 0x01, usage_v = 0x02;
  CFNumberRef upage_num = CFNumberCreate(NULL, kCFNumberIntType, &upage_v);
  CFNumberRef usage_num = CFNumberCreate(NULL, kCFNumberIntType, &usage_v);

  for (size_t i = 0; i < MAC_KNOWN_TRACKPAD_COUNT; i++) {
    int vid = MAC_KNOWN_TRACKPADS[i].vid;
    int pid = MAC_KNOWN_TRACKPADS[i].pid;
    CFNumberRef vid_num = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef pid_num = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = {
      CFSTR(kIOHIDVendorIDKey),
      CFSTR(kIOHIDProductIDKey),
      CFSTR(kIOHIDPrimaryUsagePageKey),
      CFSTR(kIOHIDPrimaryUsageKey),
    };
    const void *vals[] = { vid_num, pid_num, upage_num, usage_num };
    CFDictionaryRef dict = CFDictionaryCreate(
      NULL, keys, vals, 4,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayAppendValue(match_arr, dict);
    CFRelease(dict);
    CFRelease(vid_num);
    CFRelease(pid_num);
  }
  CFRelease(upage_num);
  CFRelease(usage_num);

  IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault,
                                           kIOHIDOptionsTypeNone);
  IOHIDManagerSetDeviceMatchingMultiple(mgr, match_arr);
  CFRelease(match_arr);

  if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
    CFRelease(mgr);
    return TCL_OK;
  }

  CFSetRef devices = IOHIDManagerCopyDevices(mgr);
  if (!devices) {
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return TCL_OK;
  }

  CFIndex count = CFSetGetCount(devices);
  if (count > 0) {
    IOHIDDeviceRef *devs =
      (IOHIDDeviceRef *) malloc(sizeof(IOHIDDeviceRef) * count);
    CFSetGetValues(devices, (const void **) devs);

    for (CFIndex i = 0; i < count; i++) {
      if (class_has_open_device(st, cls)) break;  /* v1: at most one trackpad */
      input_device_t *d = open_device_from_hid(st, cls, devs[i]);
      if (!d) continue;
      if (start_device(d) < 0) {
        unlink_and_free_device(st, d);
        continue;
      }
      if (result_dict && interp) {
        Tcl_Obj *info = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, info,
                       Tcl_NewStringObj("path", -1),
                       Tcl_NewStringObj(d->path, -1));
        Tcl_DictObjPut(interp, info,
                       Tcl_NewStringObj("datapoint", -1),
                       Tcl_NewStringObj(d->point_name, -1));
        Tcl_DictObjPut(interp, result_dict,
                       Tcl_NewStringObj(cls->name, -1),
                       info);
      }
    }
    free(devs);
  }

  CFRelease(devices);
  IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
  CFRelease(mgr);
  return TCL_OK;
}

#endif /* __linux__ */

/*****************************************************************************
 * Tcl commands
 *****************************************************************************/

static int input_autodiscover_cmd(ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;
  (void)objc; (void)objv;

  Tcl_Obj *result = Tcl_NewDictObj();
  if (autodiscover_impl(st, interp, result) != TCL_OK) {
    Tcl_DecrRefCount(result);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

static int input_open_cmd(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;

  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "class path");
    return TCL_ERROR;
  }

  input_class_t *cls = find_class(st, Tcl_GetString(objv[1]));
  if (!cls) {
    Tcl_AppendResult(interp, "input: unknown class ",
                     Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }

  input_device_t *d = open_device(st, cls, Tcl_GetString(objv[2]), interp);
  if (!d) return TCL_ERROR;
  if (start_device(d) < 0) {
    Tcl_AppendResult(interp, "input: failed to start reader thread", NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

static int input_known_device_cmd(ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;

  if (objc < 3 || (objc % 2) != 1) {
    Tcl_WrongNumArgs(interp, 1, objv,
                     "class pattern ?-screen_w W? ?-screen_h H? "
                     "?-rotation R? ?-track_drag {0|1}? ?-hdmi_output name?");
    return TCL_ERROR;
  }

  if (!find_class(st, Tcl_GetString(objv[1]))) {
    Tcl_AppendResult(interp, "input: unknown class ",
                     Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }

  if (st->n_known >= INPUT_MAX_KNOWN_DEVICES) {
    Tcl_AppendResult(interp, "input: too many known-device entries", NULL);
    return TCL_ERROR;
  }

  known_device_t *k = &st->known[st->n_known];
  memset(k, 0, sizeof(*k));
  strncpy(k->class_name, Tcl_GetString(objv[1]), sizeof(k->class_name) - 1);
  strncpy(k->pattern, Tcl_GetString(objv[2]), sizeof(k->pattern) - 1);
  k->screen_w = -1;
  k->screen_h = -1;
  k->rotation = -2;
  k->track_drag = -1;

  for (int i = 3; i < objc; i += 2) {
    const char *key = Tcl_GetString(objv[i]);
    Tcl_Obj *val = objv[i + 1];
    int ival;
    if (strcmp(key, "-screen_w") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      k->screen_w = ival;
    } else if (strcmp(key, "-screen_h") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      k->screen_h = ival;
    } else if (strcmp(key, "-rotation") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      k->rotation = ival;
    } else if (strcmp(key, "-track_drag") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      k->track_drag = ival;
    } else if (strcmp(key, "-hdmi_output") == 0) {
      strncpy(k->hdmi_output, Tcl_GetString(val),
              sizeof(k->hdmi_output) - 1);
    } else {
      Tcl_AppendResult(interp, "input: unknown option ", key, NULL);
      return TCL_ERROR;
    }
  }

  st->n_known++;
  return TCL_OK;
}

static int input_configure_cmd(ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;

  if (objc < 2 || (objc % 2) != 0) {
    Tcl_WrongNumArgs(interp, 1, objv,
                     "class ?-rotation N? ?-screen_w W? ?-screen_h H? "
                     "?-track_drag {0|1}? ?-hdmi_output name?");
    return TCL_ERROR;
  }

  input_class_t *cls = find_class(st, Tcl_GetString(objv[1]));
  if (!cls) {
    Tcl_AppendResult(interp, "input: unknown class ",
                     Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }

  for (int i = 2; i < objc; i += 2) {
    const char *key = Tcl_GetString(objv[i]);
    Tcl_Obj *val = objv[i + 1];
    int ival;

    if (strcmp(key, "-rotation") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      cls->rotation = ival;
    } else if (strcmp(key, "-screen_w") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      cls->screen_width = ival;
    } else if (strcmp(key, "-screen_h") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      cls->screen_height = ival;
    } else if (strcmp(key, "-track_drag") == 0) {
      if (Tcl_GetIntFromObj(interp, val, &ival) != TCL_OK) return TCL_ERROR;
      cls->track_drag = ival;
    } else if (strcmp(key, "-hdmi_output") == 0) {
      strncpy(cls->hdmi_output, Tcl_GetString(val),
              sizeof(cls->hdmi_output) - 1);
    } else {
      Tcl_AppendResult(interp, "input: unknown option ", key, NULL);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

static int input_expect_cmd(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;

  if (objc < 2 || objc > 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "class ?-optional?");
    return TCL_ERROR;
  }

  int required = 1;
  if (objc == 3) {
    const char *flag = Tcl_GetString(objv[2]);
    if (strcmp(flag, "-optional") == 0) {
      required = 0;
    } else {
      Tcl_AppendResult(interp, "input: unknown option ", flag, NULL);
      return TCL_ERROR;
    }
  }

  if (!find_class(st, Tcl_GetString(objv[1]))) {
    Tcl_AppendResult(interp, "input: unknown class ",
                     Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }

  if (st->n_expects >= INPUT_MAX_EXPECTS) {
    Tcl_AppendResult(interp, "input: too many expectations", NULL);
    return TCL_ERROR;
  }

  strncpy(st->expects[st->n_expects].class_name,
          Tcl_GetString(objv[1]), 31);
  st->expects[st->n_expects].required = required;
  st->n_expects++;
  return TCL_OK;
}

static int input_validate_expectations_cmd(ClientData data, Tcl_Interp *interp,
                                           int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;
  (void)objc; (void)objv;

  for (int i = 0; i < st->n_expects; i++) {
    input_expect_t *e = &st->expects[i];
    if (!e->required) continue;
    input_class_t *cls = find_class(st, e->class_name);
    if (!cls || !class_has_open_device(st, cls)) {
      Tcl_AppendResult(interp,
                       "input: required device class not found: ",
                       e->class_name, NULL);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

static int input_list_cmd(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;
  (void)objc; (void)objv;

  Tcl_Obj *list = Tcl_NewListObj(0, NULL);
  pthread_mutex_lock(&st->devices_lock);
  for (input_device_t *d = st->devices; d; d = d->next) {
    Tcl_Obj *entry = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, entry,
                   Tcl_NewStringObj("class", -1),
                   Tcl_NewStringObj(d->cls->name, -1));
    Tcl_DictObjPut(interp, entry,
                   Tcl_NewStringObj("path", -1),
                   Tcl_NewStringObj(d->path, -1));
    Tcl_DictObjPut(interp, entry,
                   Tcl_NewStringObj("datapoint", -1),
                   Tcl_NewStringObj(d->point_name, -1));
    Tcl_ListObjAppendElement(interp, list, entry);
  }
  pthread_mutex_unlock(&st->devices_lock);

  Tcl_SetObjResult(interp, list);
  return TCL_OK;
}

static int input_close_cmd(ClientData data, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;
  (void)interp; (void)objc; (void)objv;
  destroy_all_devices(st);
  return TCL_OK;
}

/* Diagnostic: enumerate every /dev/input/event* device with identifiers,
   classification result, and the capability bits we classify on. Does not
   open readers or publish anything. Useful for figuring out why a device
   isn't being picked up — or for writing the right inputKnownDevice
   pattern for new hardware. */
static int input_probe_cmd(ClientData data, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  input_state_t *st = (input_state_t *) data;
  (void)objc; (void)objv;

  Tcl_Obj *result = Tcl_NewListObj(0, NULL);

#ifdef __linux__
  DIR *dir = opendir("/dev/input");
  if (!dir) {
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;

    char path[INPUT_PATH_MAX];
    snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); continue; }

    const char *classified = "unclassified";
    for (int i = 0; i < st->n_classes; i++) {
      if (st->classes[i].matches && st->classes[i].matches(dev)) {
        classified = st->classes[i].name;
        break;
      }
    }

    const char *lib_name = libevdev_get_name(dev);
    char by_id[INPUT_PATH_MAX] = "";
    find_by_id_name(path, by_id, sizeof(by_id));

    Tcl_Obj *info = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("path", -1),
                   Tcl_NewStringObj(path, -1));
    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("class", -1),
                   Tcl_NewStringObj(classified, -1));
    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("name", -1),
                   Tcl_NewStringObj(lib_name ? lib_name : "", -1));
    if (*by_id) {
      Tcl_DictObjPut(interp, info, Tcl_NewStringObj("by_id", -1),
                     Tcl_NewStringObj(by_id, -1));
    }

    Tcl_Obj *caps = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, caps, Tcl_NewStringObj("INPUT_PROP_DIRECT", -1),
                   Tcl_NewIntObj(
                     libevdev_has_property(dev, INPUT_PROP_DIRECT)));
    Tcl_DictObjPut(interp, caps, Tcl_NewStringObj("INPUT_PROP_POINTER", -1),
                   Tcl_NewIntObj(
                     libevdev_has_property(dev, INPUT_PROP_POINTER)));
    Tcl_DictObjPut(interp, caps, Tcl_NewStringObj("ABS_X", -1),
                   Tcl_NewIntObj(
                     libevdev_has_event_code(dev, EV_ABS, ABS_X)));
    Tcl_DictObjPut(interp, caps, Tcl_NewStringObj("ABS_MT_POSITION_X", -1),
                   Tcl_NewIntObj(
                     libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X)));
    Tcl_DictObjPut(interp, caps, Tcl_NewStringObj("BTN_TOUCH", -1),
                   Tcl_NewIntObj(
                     libevdev_has_event_code(dev, EV_KEY, BTN_TOUCH)));
    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("caps", -1), caps);

    /* Axis ranges for the axes this device actually exposes — helpful
       for picking sensible -screen_w/-screen_h in inputKnownDevice. */
    Tcl_Obj *axes = Tcl_NewDictObj();
    if (libevdev_has_event_code(dev, EV_ABS, ABS_X)) {
      Tcl_Obj *r = Tcl_NewListObj(0, NULL);
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_minimum(dev, ABS_X)));
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_maximum(dev, ABS_X)));
      Tcl_DictObjPut(interp, axes, Tcl_NewStringObj("ABS_X", -1), r);
    }
    if (libevdev_has_event_code(dev, EV_ABS, ABS_Y)) {
      Tcl_Obj *r = Tcl_NewListObj(0, NULL);
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_minimum(dev, ABS_Y)));
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_maximum(dev, ABS_Y)));
      Tcl_DictObjPut(interp, axes, Tcl_NewStringObj("ABS_Y", -1), r);
    }
    if (libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X)) {
      Tcl_Obj *r = Tcl_NewListObj(0, NULL);
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_minimum(dev, ABS_MT_POSITION_X)));
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_maximum(dev, ABS_MT_POSITION_X)));
      Tcl_DictObjPut(interp, axes,
                     Tcl_NewStringObj("ABS_MT_POSITION_X", -1), r);
    }
    if (libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y)) {
      Tcl_Obj *r = Tcl_NewListObj(0, NULL);
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_minimum(dev, ABS_MT_POSITION_Y)));
      Tcl_ListObjAppendElement(interp, r,
          Tcl_NewIntObj(libevdev_get_abs_maximum(dev, ABS_MT_POSITION_Y)));
      Tcl_DictObjPut(interp, axes,
                     Tcl_NewStringObj("ABS_MT_POSITION_Y", -1), r);
    }
    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("axes", -1), axes);

    Tcl_ListObjAppendElement(interp, result, info);

    libevdev_free(dev);
    close(fd);
  }

  closedir(dir);
#else
  (void)st;
#endif

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*****************************************************************************
 * Class registration (internal, at module init)
 *****************************************************************************/

static input_class_t *register_class(input_state_t *st,
                                     const char *name,
                                     const char *datapoint,
                                     input_match_fn matches,
                                     input_reader_fn reader)
{
  if (st->n_classes >= INPUT_MAX_CLASSES) return NULL;
  input_class_t *c = &st->classes[st->n_classes++];
  strncpy(c->name, name, sizeof(c->name) - 1);
  strncpy(c->datapoint, datapoint, sizeof(c->datapoint) - 1);
  c->matches = matches;
  c->reader = reader;
  c->rotation = -1;  /* -1 = auto-detect from HDMI */
  c->screen_width = 0;
  c->screen_height = 0;
  c->track_drag = 0;
  strncpy(c->hdmi_output, "HDMI-A-1", sizeof(c->hdmi_output) - 1);
  return c;
}

static void input_cleanup(ClientData data, Tcl_Interp *interp)
{
  (void)interp;
  input_state_t *st = (input_state_t *) data;
  destroy_all_devices(st);
  pthread_mutex_destroy(&st->devices_lock);
  free(st);
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int, Dserv_input_Init) (Tcl_Interp *interp)
#else
int Dserv_input_Init(Tcl_Interp *interp)
#endif
{
  if (
#ifdef USE_TCL_STUBS
      Tcl_InitStubs(interp, "8.6-", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  input_state_t *st = (input_state_t *) calloc(1, sizeof(*st));
  if (!st) return TCL_ERROR;

  st->tclserver = tclserver_get_from_interp(interp);
  pthread_mutex_init(&st->devices_lock, NULL);

  register_class(st, "touchscreen", "mtouch/event",
                 touchscreen_matches, touchscreen_reader);
  register_class(st, "trackpad", "mtouch/trackpad",
                 trackpad_matches, trackpad_reader);

  Tcl_CreateObjCommand(interp, "inputAutodiscover",
                       input_autodiscover_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputOpen",
                       input_open_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputConfigure",
                       input_configure_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputKnownDevice",
                       input_known_device_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputExpect",
                       input_expect_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputValidateExpectations",
                       input_validate_expectations_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputList",
                       input_list_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputProbe",
                       input_probe_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputClose",
                       input_close_cmd, st, NULL);

  Tcl_CallWhenDeleted(interp, input_cleanup, st);

  return TCL_OK;
}
