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

#ifdef __linux__
#include <libevdev/libevdev.h>
#include <linux/input.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

#define INPUT_MAX_CLASSES    8
#define INPUT_MAX_EXPECTS    8
#define INPUT_PATH_MAX       256
#define INPUT_DPOINT_MAX     64

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

typedef struct input_state_s {
  tclserver_t *tclserver;

  input_class_t classes[INPUT_MAX_CLASSES];
  int n_classes;

  input_device_t *devices;
  pthread_mutex_t devices_lock;

  input_expect_t expects[INPUT_MAX_EXPECTS];
  int n_expects;
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

  return NULL;
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
 *   mtouch/trackpad/range   int32[4]  (min_x, max_x, min_y, max_y) once at open
 *
 * Event semantics mirror touchscreen (0 = PRESS, 1 = DRAG, 2 = RELEASE).
 * Coordinates are raw trackpad surface units; the slider subprocess maps
 * them to stimulus space using the range datapoint. v1 tracks slot 0 only.
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

  int32_t vals[4];
  vals[0] = d->minx;
  vals[1] = d->maxx;
  vals[2] = d->miny;
  vals[3] = d->maxy;
  ds_datapoint_t *dp = dpoint_new(range_name,
                                  tclserver_now(d->state->tclserver),
                                  DSERV_INT,
                                  sizeof(vals),
                                  (unsigned char *) vals);
  tclserver_set_point(d->state->tclserver, dp);
}

static void *trackpad_reader(void *arg)
{
  input_device_t *d = (input_device_t *) arg;
  struct input_event ev;
  int current_slot = 0;
  int slot0_active = 0;
  int slot0_changed = 0;
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
        if (current_slot == 0) {
          if (ev.value >= 0) {
            slot0_active = 1;
            slot0_changed = 1;
            first_coord_after_press = 0;
          } else {
            slot0_active = 0;
            slot0_changed = 1;
          }
        }
        break;

      case ABS_MT_POSITION_X:
        if (current_slot == 0) { x = ev.value; coords_changed = 1; }
        break;

      case ABS_MT_POSITION_Y:
        if (current_slot == 0) { y = ev.value; coords_changed = 1; }
        break;
      }
      break;

    case EV_SYN:
      if (ev.code != SYN_REPORT) break;

      if (slot0_active && (coords_changed || slot0_changed)) {
        uint16_t vals[3];
        vals[0] = (uint16_t) x;
        vals[1] = (uint16_t) y;

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
      } else if (!slot0_active && slot0_changed) {
        uint16_t vals[3];
        vals[0] = (uint16_t) x;
        vals[1] = (uint16_t) y;
        vals[2] = 2;  /* RELEASE */
        ds_datapoint_t *dp = dpoint_new(d->point_name,
                                        tclserver_now(d->state->tclserver),
                                        DSERV_SHORT,
                                        sizeof(vals),
                                        (unsigned char *) vals);
        tclserver_set_point(d->state->tclserver, dp);
      }

    sync_done_tp:
      slot0_changed = 0;
      coords_changed = 0;
      break;
    }
  } while (rc >= 0);

  return NULL;
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

static int autodiscover_impl(input_state_t *st, Tcl_Interp *interp,
                             Tcl_Obj *result_dict)
{
  DIR *dir = opendir("/dev/input");
  if (!dir) return TCL_OK;  /* no devices; not fatal */

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;

    char path[INPUT_PATH_MAX];
    snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
      close(fd);
      continue;
    }

    /* Pick the first class whose predicate matches and that doesn't
       already have an open device. Phase 1 is single-device-per-class. */
    input_class_t *matched = NULL;
    for (int i = 0; i < st->n_classes; i++) {
      input_class_t *c = &st->classes[i];
      if (c->matches && c->matches(dev) && !class_has_open_device(st, c)) {
        matched = c;
        break;
      }
    }

    libevdev_free(dev);
    close(fd);

    if (!matched) continue;

    input_device_t *d = open_device(st, matched, path, interp);
    if (!d) continue;
    if (start_device(d) < 0) {
      /* Unlink and free; open_device already put it in the list. */
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
      continue;
    }

    if (result_dict) {
      Tcl_DictObjPut(interp, result_dict,
                     Tcl_NewStringObj(matched->name, -1),
                     Tcl_NewStringObj(path, -1));
    }
  }

  closedir(dir);
  return TCL_OK;
}

#else /* !__linux__ */

/*****************************************************************************
 * macOS / non-Linux stubs
 *****************************************************************************/

static int touchscreen_matches(void *dev) { (void)dev; return 0; }
static void *touchscreen_reader(void *arg) { (void)arg; return NULL; }
static int trackpad_matches(void *dev) { (void)dev; return 0; }
static void *trackpad_reader(void *arg) { (void)arg; return NULL; }

static input_device_t *open_device(input_state_t *st, input_class_t *cls,
                                   const char *path, Tcl_Interp *interp)
{
  (void)st; (void)cls; (void)path; (void)interp;
  return NULL;
}
static int start_device(input_device_t *d) { (void)d; return 0; }
static void stop_device(input_device_t *d) { (void)d; }
static void destroy_all_devices(input_state_t *st) { (void)st; }
static int autodiscover_impl(input_state_t *st, Tcl_Interp *interp,
                             Tcl_Obj *result_dict)
{
  (void)st; (void)interp; (void)result_dict;
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
  Tcl_CreateObjCommand(interp, "inputExpect",
                       input_expect_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputValidateExpectations",
                       input_validate_expectations_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputList",
                       input_list_cmd, st, NULL);
  Tcl_CreateObjCommand(interp, "inputClose",
                       input_close_cmd, st, NULL);

  Tcl_CallWhenDeleted(interp, input_cleanup, st);

  return TCL_OK;
}
