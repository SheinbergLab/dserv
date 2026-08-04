/*
 * NAME
 *   sound.c
 *
 * DESCRIPTION
 *   Sound module for dserv: task signals and wav stimuli through one
 *   audio output path.
 *
 *   Two signal backends (combinable, mode bitmask):
 *     HARDWARE - MIDI over serial to an external synth (SW60XG / MU series)
 *     SOFTWARE - FluidSynth rendered in-process
 *
 *   The audio device is owned by this module via miniaudio.  FluidSynth no
 *   longer opens its own driver: the miniaudio callback pulls samples with
 *   fluid_synth_write_float() and mixes preloaded wav stimuli on top, so
 *   beeps and stimuli share one device with sample-accurate relative timing
 *   and no dependency on system-level mixing (dmix/PipeWire).
 *
 *   Wav stimuli are decoded (and resampled to the device rate) once at
 *   wavLoad time and mixed from memory at wavPlay time.  Onset/offset are
 *   published as datapoints (sound/wav/onset, sound/wav/offset) from the
 *   audio callback; note the published time is when the block was rendered,
 *   which leads actual DAC output by the device buffer (~2 periods).
 *
 *   Scheduled note-offs for soundPlay are counted down in frames inside the
 *   callback (sub-block rendering makes durations sample-accurate).  If no
 *   audio device is running (hardware-MIDI-only rigs), note-offs fall back
 *   to a detached timing thread.
 *
 * AUTHOR
 *   DLS, 06/24, 08/26
 */

/* RTLD_DEFAULT is a GNU extension on glibc; must precede all includes */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#ifndef WIN32
#include <sys/ioctl.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <dlfcn.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

/* FluidSynth for software synthesis */
#include <fluidsynth.h>

/* miniaudio owns the output device and decodes wav stimuli */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* ALSA retained only for the soundListAlsaDevices command on Linux */
#if !defined(__APPLE__) && !defined(WIN32)
#include <alsa/asoundlib.h>
#endif

/*************************************************************************/
/***                     Sound mode selection                          ***/
/*************************************************************************/

typedef enum {
  SOUND_MODE_NONE =     0x00,  /* Not initialized */
  SOUND_MODE_HARDWARE = 0x01,  /* MIDI over serial to hardware synth */
  SOUND_MODE_SOFTWARE = 0x02,  /* FluidSynth software synthesis */
  SOUND_MODE_BOTH =     0x03,
} sound_mode_t;

/*************************************************************************/
/***                     audio output configuration                    ***/
/*************************************************************************/

#define AO_SAMPLE_RATE    48000
#define AO_CHANNELS       2
#define AO_PERIOD_FRAMES  256
#define AO_PERIODS        2

#define MAX_WAVS          64   /* loaded wav table               */
#define MAX_WAV_VOICES    16   /* simultaneously playing stimuli */
#define MAX_PENDING_OFFS  64   /* scheduled note-offs in flight  */
#define WAV_NAME_MAX      64

/* datapoint names */
#define PT_WAV_LOADED   "sound/wav/loaded"
#define PT_WAV_ONSET    "sound/wav/onset"
#define PT_WAV_OFFSET   "sound/wav/offset"
#define PT_AUDIO_DEVICE "sound/audio/device"

/*************************************************************************/
/***                            MIDI related info                      ***/
/*************************************************************************/

#define MIDI_OFF 0
#define MIDI_ON 64

#define MIDI_VOICES         0
#define MIDI_SFX           64
#define MIDI_DRUMS        127

#define MIDI_CTRL_VOLUME    7
#define MIDI_CTRL_HOLD     64
#define MIDI_CTRL_SUSTENTO 66

/* Set array for program change message */
static char Sets[] = { MIDI_VOICES, MIDI_DRUMS, MIDI_SFX };

/*************************************************************************/
/***                            data structures                        ***/
/*************************************************************************/

/* loaded wav stimulus: interleaved stereo f32 at AO_SAMPLE_RATE */
typedef struct wav_entry_s {
  char name[WAV_NAME_MAX];
  float *pcm;
  ma_uint64 nframes;
  int used;
} wav_entry_t;

/* voice states: claimed/advanced by the Tcl thread only via FREE->START
 * and (START|PLAY)->STOPREQ; the audio callback owns every other
 * transition and is the only reader of pcm/pos while active. */
enum {
  WAV_VOICE_FREE    = 0,
  WAV_VOICE_START   = 1,
  WAV_VOICE_PLAY    = 2,
  WAV_VOICE_STOPREQ = 3,
};

typedef struct wav_voice_s {
  _Atomic int state;
  int wav_index;
  char name[WAV_NAME_MAX];
  const float *pcm;
  ma_uint64 nframes;
  ma_uint64 pos;
  float gain;
  int loop;
} wav_voice_t;

/* scheduled note-off, counted down in frames by the audio callback */
typedef struct pending_off_s {
  _Atomic int armed;
  int channel;
  int pitch;
  ma_uint64 frames_remaining;
} pending_off_t;

/* host (dserv) datapoint API, resolved at load time so the module also
 * loads into a plain tclsh (publishing simply disabled there) */
typedef struct host_api_s {
  tclserver_t *(*get_from_interp)(Tcl_Interp *interp);
  uint64_t (*now)(tclserver_t *);
  void (*set_point)(tclserver_t *, ds_datapoint_t *);
  ds_datapoint_t *(*new_point)(char *, uint64_t, ds_datatype_t,
                               uint32_t, unsigned char *);
} host_api_t;

typedef struct sound_info_s
{
  sound_mode_t mode;

  /* Hardware mode (MIDI over serial) */
  int midi_fd;

  /* Software mode (FluidSynth, rendered by the audio callback) */
  fluid_settings_t *settings;
  fluid_synth_t *synth;                   /* owned by the Tcl thread     */
  _Atomic(fluid_synth_t *) render_synth;  /* what the callback renders   */

  /* audio output (miniaudio) */
  ma_device ao_device;
  int ao_initialized;
  _Atomic int ao_running;
  char ao_name[256];

  _Atomic float master_gain;              /* post-mix, 0.0 - 1.0 */

  wav_entry_t wavs[MAX_WAVS];
  wav_voice_t voices[MAX_WAV_VOICES];
  pending_off_t pending_offs[MAX_PENDING_OFFS];

  tclserver_t *tclserver;
  host_api_t api;
} sound_info_t;

static int snd_on(sound_info_t *, char channel, char pitch);
static int snd_off(sound_info_t *, char channel, char pitch);
static int snd_control(sound_info_t *, char control, char data, char channel);
static int snd_program(sound_info_t *, char program, char bank, char ch_set);
static int snd_reset(sound_info_t *);
static int snd_volume(sound_info_t *, char volume, char channel);
static int snd_master_gain(sound_info_t *, double level);
static int audio_out_ensure(sound_info_t *info, const char *pattern,
                            char *err, size_t errsz);

/*************************************************************************/
/***                      datapoint publishing                         ***/
/*************************************************************************/

static void publish_string(sound_info_t *info, const char *varname,
                           const char *val)
{
  if (!info->tclserver || !info->api.set_point ||
      !info->api.now || !info->api.new_point)
    return;
  ds_datapoint_t *dp = info->api.new_point((char *) varname,
                                           info->api.now(info->tclserver),
                                           DSERV_STRING,
                                           (uint32_t) strlen(val),
                                           (unsigned char *) val);
  if (dp) info->api.set_point(info->tclserver, dp);
}

/*************************************************************************/
/***                     MIDI backend primitives                       ***/
/*************************************************************************/

/**************************************************************
 *
 * FUNCTION
 *   snd_program
 *
 * DESCRIPTION
 *   Send a program change message to the midi driver
 * or fluidsynth via software.
 *
 * Note that the channel and set are packed into the
 * ch_set variable, since channels are only between 0-15
 * and there are only 3 sets (VOICES, DRUMS, SFX).
 *
 **************************************************************/

static int snd_program(sound_info_t *info, char program, char bank, char ch_set)
{
  char set, channel;
  int n = 0;

  channel = ch_set & 0x0F;	/* Low nibble  */
  set = (ch_set & 0xF0) >> 4; /* High nibble */

  if (set > sizeof(Sets)) return 0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send MIDI over serial */
    char cmd[8];

    /* Start with Channel change 0 */
    cmd[0] = 0xb0 | channel;
    cmd[1] = 0x00;
    cmd[2] = Sets[(int) set];

    /* Here's the LSB (Bank Select) command */
    cmd[3] = 0xb0 | channel;
    cmd[4] = 0x20;
    cmd[5] = bank;

    /* Here's the program change */
    cmd[6] = 0xc0 | channel;
    cmd[7] = program-1;

    if (info->midi_fd >= 0)
      n = write(info->midi_fd, cmd, sizeof(cmd));

    /* Now set the volume to the middle */
    snd_control(info, MIDI_CTRL_VOLUME, 64, channel);
  }

  if (info->mode & SOUND_MODE_SOFTWARE) {
    if (info->synth) {
      /* Bank select MSB (CC 0) */
      fluid_synth_cc(info->synth, channel, 0, Sets[(int)set]);

      /* Bank select LSB (CC 32) */
      fluid_synth_cc(info->synth, channel, 32, bank);

      /* Program change */
      fluid_synth_program_change(info->synth, channel, program - 1);

      /* Set volume to middle */
      fluid_synth_cc(info->synth, channel, MIDI_CTRL_VOLUME, 64);

      n = 1;
    }
  }

  return n;
}

/**************************************************************
 *
 * FUNCTION
 *   snd_volume
 *
 * DESCRIPTION
 *   Send a volume change message to the midi driver
 *
 **************************************************************/

static int snd_volume(sound_info_t *info, char volume, char channel)
{
  return snd_control(info, MIDI_CTRL_VOLUME, volume, channel);
}

/**************************************************************
 *
 * FUNCTION
 *   snd_control
 *
 * DESCRIPTION
 *   Send a control change message to the midi driver
 *
 **************************************************************/

static int snd_control(sound_info_t *info, char control, char data, char channel)
{
  int result = 0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send MIDI over serial */
    char cmd[3];
    cmd[0] = 0xb0 | channel;
    cmd[1] = control;
    cmd[2] = data;

    if (info->midi_fd >= 0)
      result = write(info->midi_fd, cmd, sizeof(cmd));
  }

  if (info->mode & SOUND_MODE_SOFTWARE) {
    if (info->synth)
      result = fluid_synth_cc(info->synth, channel, control, data);
  }

  return result;
}

static int snd_reset(sound_info_t *info)
{
  int n = 0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send XG reset SysEx */
    static char xg_on[] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00,
      0x7e, 0x00, 0xf7 };
    static char master_volume[] = { 0xf0, 0x7f, 0x7f, 0x04, 0x01,
      0x7f, 0x7f, 0xf7 };

    if (info->midi_fd >= 0)
      write(info->midi_fd, xg_on, sizeof(xg_on));

    /* according to the MU15 docs, the xg_on command takes approx 50ms */
    usleep(50000);

    if (info->midi_fd >= 0) {
      n = write(info->midi_fd, master_volume, sizeof(master_volume));
      /* both sysex fully on the wire before anything else follows: a
       * partial XG-ON (no F7) leaves the synth eating every later byte
       * as sysex data -- silence with a healthy-looking port */
      tcdrain(info->midi_fd);
    }
  }

  if (info->mode & SOUND_MODE_SOFTWARE) {
    if (info->synth) {
      fluid_synth_system_reset(info->synth);
      n = 1;
    }
  }

  return n;
}

/**************************************************************
 *
 * FUNCTION
 *   snd_master_gain
 *
 * DESCRIPTION
 *   Set the overall output level, independent of the per-channel MIDI
 *   volumes. `level` is normalized 0.0 (silent) .. 1.0 (full). 1.0
 *   reproduces prior behavior on both backends (FluidSynth's default gain /
 *   the hardware's max master volume), so values below 1.0 attenuate the
 *   whole rig -- e.g. to turn a setup down for a housing room without
 *   touching the experiment's per-channel mix.
 *
 *   With the unified output path the software-side gain is applied to the
 *   final mix in the audio callback, so it governs wav stimuli as well as
 *   synth output (matching the documented "how loud is this rig" intent).
 *
 **************************************************************/

/* FluidSynth gain corresponding to level 1.0. Matches FluidSynth's own
 * default synth.gain, so a master of 1.0 leaves loudness unchanged from
 * before this control existed. Raise it if a rig needs more headroom. */
#define SND_FLUID_FULL_GAIN 0.2

static int snd_master_gain(sound_info_t *info, double level)
{
  int result = 0;

  if (level < 0.0) level = 0.0;
  if (level > 1.0) level = 1.0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Universal Real Time SysEx: Master Volume (14-bit, LSB then MSB) */
    int v = (int)(level * 16383.0 + 0.5);
    char master_volume[] = { 0xf0, 0x7f, 0x7f, 0x04, 0x01,
      (char)(v & 0x7f), (char)((v >> 7) & 0x7f), 0xf7 };
    if (info->midi_fd >= 0)
      result = write(info->midi_fd, master_volume, sizeof(master_volume));
  }

  atomic_store_explicit(&info->master_gain, (float) level,
                        memory_order_relaxed);
  if ((info->mode & SOUND_MODE_SOFTWARE) || info->ao_initialized)
    result = 1;

  return result;
}

/**************************************************************
 * FUNCTION
 *   snd_on
 *
 * DESCRIPTION
 *   Turn on the sound for the specified channel
 *
 **************************************************************/

static int snd_on(sound_info_t *info, char channel, char pitch)
{
  static char vel = 127;
  int n = 0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send MIDI note-on over serial */
    char cmd[3];
    cmd[0] = 0x90 | channel;
    cmd[1] = pitch;
    cmd[2] = vel;

    if (info->midi_fd >= 0)
      n = write(info->midi_fd, cmd, sizeof(cmd));
  }

  if (info->mode & SOUND_MODE_SOFTWARE) {
    if (info->synth)
      n = fluid_synth_noteon(info->synth, channel, pitch, vel);
  }

  return n;
}

/**************************************************************
 * FUNCTION
 *   snd_off
 *
 * DESCRIPTION
 *   Turn off the sound for the specified channel
 *
 * NOTE
 *   Called from the Tcl thread, the audio callback (scheduled offs), or a
 *   fallback timing thread.  Serial writes are short+nonblocking and the
 *   FluidSynth API is thread-safe, so no locking is needed here.
 *
 **************************************************************/

static int snd_off(sound_info_t *info, char channel, char pitch)
{
  int n = 0;

  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send MIDI note-off over serial */
    char cmd[3];
    cmd[0] = 0x80 | channel;
    cmd[1] = pitch;
    cmd[2] = 0x40;

    if (info->midi_fd >= 0)
      n = write(info->midi_fd, cmd, sizeof(cmd));
  }

  if (info->mode & SOUND_MODE_SOFTWARE) {
    fluid_synth_t *synth =
      atomic_load_explicit(&info->render_synth, memory_order_acquire);
    if (synth)
      n = fluid_synth_noteoff(synth, channel, pitch);
  }

  return n;
}

/*************************************************************************/
/***                        audio callback                             ***/
/*************************************************************************/

static void ao_data_callback(ma_device *dev, void *output, const void *input,
                             ma_uint32 nframes)
{
  sound_info_t *info = (sound_info_t *) dev->pUserData;
  float *buf = (float *) output;
  (void) input;

  memset(buf, 0, (size_t) nframes * AO_CHANNELS * sizeof(float));

  fluid_synth_t *synth =
    atomic_load_explicit(&info->render_synth, memory_order_acquire);

  /* Render the synth in sub-blocks split at scheduled note-off
   * boundaries so soundPlay durations are sample-accurate rather than
   * period-quantized. */
  ma_uint32 done = 0;
  while (done < nframes) {
    ma_uint32 chunk = nframes - done;

    for (int i = 0; i < MAX_PENDING_OFFS; i++) {
      pending_off_t *po = &info->pending_offs[i];
      if (atomic_load_explicit(&po->armed, memory_order_acquire) != 1)
        continue;
      if (po->frames_remaining == 0) {
        snd_off(info, po->channel, po->pitch);
        atomic_store_explicit(&po->armed, 0, memory_order_release);
      } else if (po->frames_remaining < chunk) {
        chunk = (ma_uint32) po->frames_remaining;
      }
    }

    if (synth)
      fluid_synth_write_float(synth, (int) chunk,
                              buf + AO_CHANNELS * done, 0, AO_CHANNELS,
                              buf + AO_CHANNELS * done, 1, AO_CHANNELS);

    for (int i = 0; i < MAX_PENDING_OFFS; i++) {
      pending_off_t *po = &info->pending_offs[i];
      if (atomic_load_explicit(&po->armed, memory_order_relaxed) == 1)
        po->frames_remaining = (po->frames_remaining > chunk) ?
          po->frames_remaining - chunk : 0;
    }
    done += chunk;
  }

  /* Mix active wav voices */
  for (int i = 0; i < MAX_WAV_VOICES; i++) {
    wav_voice_t *v = &info->voices[i];
    int st = atomic_load_explicit(&v->state, memory_order_acquire);

    if (st == WAV_VOICE_STOPREQ) {
      publish_string(info, PT_WAV_OFFSET, v->name);
      atomic_store_explicit(&v->state, WAV_VOICE_FREE, memory_order_release);
      continue;
    }
    if (st == WAV_VOICE_START) {
      int expected = WAV_VOICE_START;
      if (atomic_compare_exchange_strong(&v->state, &expected,
                                         WAV_VOICE_PLAY)) {
        publish_string(info, PT_WAV_ONSET, v->name);
        st = WAV_VOICE_PLAY;
      } else {
        continue;               /* raced to STOPREQ; retire next block */
      }
    }
    if (st != WAV_VOICE_PLAY) continue;

    ma_uint32 remaining = nframes, at = 0;
    while (remaining) {
      ma_uint64 avail = v->nframes - v->pos;
      ma_uint32 n = (ma_uint32) ((avail < remaining) ? avail : remaining);
      const float *src = v->pcm + v->pos * AO_CHANNELS;
      float *dst = buf + (size_t) at * AO_CHANNELS;
      float g = v->gain;
      for (ma_uint32 k = 0; k < n * AO_CHANNELS; k++)
        dst[k] += src[k] * g;
      v->pos += n; at += n; remaining -= n;

      if (v->pos >= v->nframes) {
        if (v->loop) {
          v->pos = 0;
        } else {
          publish_string(info, PT_WAV_OFFSET, v->name);
          atomic_store_explicit(&v->state, WAV_VOICE_FREE,
                                memory_order_release);
          break;
        }
      }
    }
  }

  /* Master gain + hard clip */
  float g = atomic_load_explicit(&info->master_gain, memory_order_relaxed);
  for (ma_uint32 k = 0; k < nframes * AO_CHANNELS; k++) {
    float s = buf[k] * g;
    buf[k] = (s > 1.0f) ? 1.0f : ((s < -1.0f) ? -1.0f : s);
  }
}

/*************************************************************************/
/***                     audio device management                       ***/
/*************************************************************************/

/* Find a playback device whose name contains `pattern`.  On Linux a
 * pattern containing ':' (or "default") is treated as a literal ALSA
 * device id, so existing local configs like "plughw:1,0" keep working.
 * Returns a malloc'd device id (caller frees) or NULL for default. */
static ma_device_id *ao_find_device(const char *pattern,
                                    char *found, size_t found_size)
{
  if (!pattern || !*pattern) return NULL;

#if !defined(__APPLE__) && !defined(WIN32)
  if (strchr(pattern, ':') || strcmp(pattern, "default") == 0) {
    ma_device_id *did = (ma_device_id *) calloc(1, sizeof(ma_device_id));
    if (!did) return NULL;
    strncpy(did->alsa, pattern, sizeof(did->alsa) - 1);
    if (found && found_size) {
      strncpy(found, pattern, found_size - 1);
      found[found_size - 1] = '\0';
    }
    return did;
  }
#endif

  ma_context context;
  ma_device_id *result = NULL;

  if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS)
    return NULL;

  ma_device_info *devices;
  ma_uint32 count;
  if (ma_context_get_devices(&context, &devices, &count,
                             NULL, NULL) == MA_SUCCESS) {
    for (ma_uint32 i = 0; i < count; i++) {
      if (strstr(devices[i].name, pattern) != NULL) {
        result = (ma_device_id *) malloc(sizeof(ma_device_id));
        if (result) {
          *result = devices[i].id;
          if (found && found_size) {
            strncpy(found, devices[i].name, found_size - 1);
            found[found_size - 1] = '\0';
          }
        }
        break;
      }
    }
  }
  ma_context_uninit(&context);
  return result;
}

static int audio_out_ensure(sound_info_t *info, const char *pattern,
                            char *err, size_t errsz)
{
  if (info->ao_initialized) return 0;

  char found[256] = "";
  ma_device_id *did = ao_find_device(pattern, found, sizeof(found));
  if (pattern && *pattern && !did
#if !defined(__APPLE__) && !defined(WIN32)
      && !strchr(pattern, ':')
#endif
      ) {
    /* pattern given but nothing matched: fall through to default,
     * but say so */
    fprintf(stderr, "sound: no audio device matching \"%s\", using default\n",
            pattern);
  }

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = AO_CHANNELS;
  cfg.sampleRate = AO_SAMPLE_RATE;
  cfg.periodSizeInFrames = AO_PERIOD_FRAMES;
  cfg.periods = AO_PERIODS;
  cfg.dataCallback = ao_data_callback;
  cfg.pUserData = info;
#if !defined(__APPLE__) && !defined(WIN32)
  /* better USB audio compatibility (same workaround as stim2/video.c) */
  cfg.alsa.noMMap = MA_TRUE;
#endif
  if (did) cfg.playback.pDeviceID = did;

  if (ma_device_init(NULL, &cfg, &info->ao_device) != MA_SUCCESS) {
    if (err) snprintf(err, errsz, "failed to open audio device%s%s",
                      *found ? " " : "", found);
    free(did);
    return -1;
  }
  free(did);

  if (ma_device_start(&info->ao_device) != MA_SUCCESS) {
    ma_device_uninit(&info->ao_device);
    if (err) snprintf(err, errsz, "failed to start audio device");
    return -1;
  }

  strncpy(info->ao_name, info->ao_device.playback.name,
          sizeof(info->ao_name) - 1);
  info->ao_initialized = 1;
  atomic_store_explicit(&info->ao_running, 1, memory_order_release);

  publish_string(info, PT_AUDIO_DEVICE, info->ao_name);
  return 0;
}

/*************************************************************************/
/***                      FluidSynth lifecycle                         ***/
/*************************************************************************/

/* Take the synth out of the callback's view and wait out any in-flight
 * render before deleting it (bounded: one device buffer + margin). */
static void retire_synth(sound_info_t *info)
{
  if (!info->synth) return;

  atomic_store_explicit(&info->render_synth, NULL, memory_order_release);
  if (atomic_load_explicit(&info->ao_running, memory_order_acquire))
    usleep((useconds_t)
           ((AO_PERIOD_FRAMES * AO_PERIODS * 1000000ULL) / AO_SAMPLE_RATE)
           + 10000);

  delete_fluid_synth(info->synth);
  info->synth = NULL;
  if (info->settings) {
    delete_fluid_settings(info->settings);
    info->settings = NULL;
  }
  info->mode &= ~SOUND_MODE_SOFTWARE;
}

/*************************************************************************/
/***                     scheduled note-offs                           ***/
/*************************************************************************/

/* fallback for rigs with no audio device running (hardware MIDI only):
 * one detached thread per note sleeps out the duration */
typedef struct fallback_off_s {
  sound_info_t *info;
  int channel, pitch, ms;
} fallback_off_t;

static void *fallback_off_thread(void *arg)
{
  fallback_off_t *fo = (fallback_off_t *) arg;
  usleep((useconds_t) fo->ms * 1000);
  snd_off(fo->info, fo->channel, fo->pitch);
  free(fo);
  return NULL;
}

static void schedule_off(sound_info_t *info, int channel, int pitch,
                         int duration_ms)
{
  if (atomic_load_explicit(&info->ao_running, memory_order_acquire)) {
    for (int i = 0; i < MAX_PENDING_OFFS; i++) {
      pending_off_t *po = &info->pending_offs[i];
      if (atomic_load_explicit(&po->armed, memory_order_acquire) == 0) {
        po->channel = channel;
        po->pitch = pitch;
        po->frames_remaining =
          (ma_uint64) duration_ms * (AO_SAMPLE_RATE / 1000);
        atomic_store_explicit(&po->armed, 1, memory_order_release);
        return;
      }
    }
    /* table full (64 pending): end the note now rather than leak it */
    snd_off(info, channel, pitch);
    return;
  }

  fallback_off_t *fo = (fallback_off_t *) malloc(sizeof(fallback_off_t));
  if (!fo) { snd_off(info, channel, pitch); return; }
  fo->info = info; fo->channel = channel; fo->pitch = pitch;
  fo->ms = duration_ms;

  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &attr, fallback_off_thread, fo) != 0) {
    snd_off(info, channel, pitch);
    free(fo);
  }
  pthread_attr_destroy(&attr);
}

/*************************************************************************/
/***                        wav stimulus layer                         ***/
/*************************************************************************/

static int wav_find(sound_info_t *info, const char *name)
{
  for (int i = 0; i < MAX_WAVS; i++)
    if (info->wavs[i].used && strcmp(info->wavs[i].name, name) == 0)
      return i;
  return -1;
}

static int wav_voices_active(sound_info_t *info, int wav_index)
{
  int n = 0;
  for (int i = 0; i < MAX_WAV_VOICES; i++) {
    wav_voice_t *v = &info->voices[i];
    if (atomic_load_explicit(&v->state, memory_order_acquire)
        != WAV_VOICE_FREE &&
        (wav_index < 0 || v->wav_index == wav_index))
      n++;
  }
  return n;
}

/* request stop for voices playing wav_index (or all if wav_index < 0) */
static void wav_stop_voices(sound_info_t *info, int wav_index)
{
  int running = atomic_load_explicit(&info->ao_running, memory_order_acquire);
  for (int i = 0; i < MAX_WAV_VOICES; i++) {
    wav_voice_t *v = &info->voices[i];
    if (wav_index >= 0 && v->wav_index != wav_index) continue;
    if (running) {
      int st = WAV_VOICE_PLAY;
      if (!atomic_compare_exchange_strong(&v->state, &st, WAV_VOICE_STOPREQ)) {
        st = WAV_VOICE_START;
        atomic_compare_exchange_strong(&v->state, &st, WAV_VOICE_STOPREQ);
      }
    } else {
      /* no reader: retire directly */
      atomic_store_explicit(&v->state, WAV_VOICE_FREE, memory_order_release);
    }
  }
}

/* wait for voices on wav_index to drain (bounded); 0 on success */
static int wav_drain(sound_info_t *info, int wav_index, int timeout_ms)
{
  int waited = 0;
  while (wav_voices_active(info, wav_index) > 0) {
    if (!atomic_load_explicit(&info->ao_running, memory_order_acquire)) {
      wav_stop_voices(info, wav_index);
      continue;
    }
    if (waited >= timeout_ms) return -1;
    usleep(5000);
    waited += 5;
  }
  return 0;
}

/* drain an initialized decoder to interleaved stereo f32 at
 * AO_SAMPLE_RATE; uninits the decoder */
static float *wav_decode_frames(ma_decoder *dec, ma_uint64 *out_frames,
                                const char *what, char *err, size_t errsz)
{
  ma_uint64 cap = AO_SAMPLE_RATE;   /* start with 1s, grow as needed */
  float *pcm = (float *) malloc(cap * AO_CHANNELS * sizeof(float));
  ma_uint64 total = 0;

  while (pcm) {
    if (total + 4096 > cap) {
      cap *= 2;
      float *bigger = (float *) realloc(pcm, cap * AO_CHANNELS * sizeof(float));
      if (!bigger) { free(pcm); pcm = NULL; break; }
      pcm = bigger;
    }
    ma_uint64 got = 0;
    ma_result r = ma_decoder_read_pcm_frames(dec,
                                             pcm + total * AO_CHANNELS,
                                             4096, &got);
    total += got;
    if (r != MA_SUCCESS || got == 0) break;
  }
  ma_decoder_uninit(dec);

  if (!pcm) {
    if (err) snprintf(err, errsz, "out of memory decoding %s", what);
    return NULL;
  }
  if (total == 0) {
    free(pcm);
    if (err) snprintf(err, errsz, "no audio frames in %s", what);
    return NULL;
  }
  *out_frames = total;
  return pcm;
}

/* decode any miniaudio-supported file (wav/flac/mp3) to interleaved
 * stereo f32 at AO_SAMPLE_RATE */
static float *wav_decode_file(const char *path, ma_uint64 *out_frames,
                              char *err, size_t errsz)
{
  ma_decoder_config dcfg =
    ma_decoder_config_init(ma_format_f32, AO_CHANNELS, AO_SAMPLE_RATE);
  ma_decoder dec;

  if (ma_decoder_init_file(path, &dcfg, &dec) != MA_SUCCESS) {
    if (err) snprintf(err, errsz, "could not open/decode \"%s\"", path);
    return NULL;
  }
  return wav_decode_frames(&dec, out_frames, path, err, errsz);
}

/* decode an in-memory sound file image (same formats as wav_decode_file);
 * the caller's buffer only needs to stay valid for the duration of the
 * call -- decoded PCM is our own allocation */
static float *wav_decode_memory(const void *data, size_t nbytes,
                                ma_uint64 *out_frames,
                                char *err, size_t errsz)
{
  ma_decoder_config dcfg =
    ma_decoder_config_init(ma_format_f32, AO_CHANNELS, AO_SAMPLE_RATE);
  ma_decoder dec;

  if (ma_decoder_init_memory(data, nbytes, &dcfg, &dec) != MA_SUCCESS) {
    if (err) snprintf(err, errsz,
                      "could not decode %zu-byte sound data", nbytes);
    return NULL;
  }
  return wav_decode_frames(&dec, out_frames, "sound data", err, errsz);
}

/*************************************************************************/
/***                      serial port (hardware)                       ***/
/*************************************************************************/

static int configure_serial_port(int fd)
{
  struct termios ser;
  tcflush(fd,TCIFLUSH);
  tcflush(fd,TCOFLUSH);
  int res = tcgetattr(fd, &ser);
  if (res < 0) {
    return -1;
  }
  cfmakeraw(&ser);
  cfsetspeed(&ser,B38400);
  if ((res = tcsetattr(fd, TCSANOW, &ser)) < 0){
    return -2;
  }
  return 0;
}

/*************************************************************************/
/***                          Tcl commands                             ***/
/*************************************************************************/

static int sound_open_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  if (info->midi_fd >= 0) close(info->midi_fd);

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "port");
    return TCL_ERROR;
  }

  info->midi_fd = open(Tcl_GetString(objv[1]), O_NOCTTY | O_NONBLOCK | O_RDWR);

  if (info->midi_fd < 0) {
    Tcl_AppendResult(interp,
		     Tcl_GetString(objv[0]), ": error opening port \"",
		     Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }
  int ret = configure_serial_port(info->midi_fd);
  if (ret < 0) {
    /* Never latch HARDWARE mode on a half-configured port: it plays
     * nothing but looks alive (office-stim 2026-08-04). */
    close(info->midi_fd);
    info->midi_fd = -1;
    Tcl_AppendResult(interp,
		     Tcl_GetString(objv[0]), ": failed to configure port \"",
		     Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }

#ifndef WIN32
  /* Deliberate DTR/RTS cycle + settle.  DIN-MIDI adapters are commonly
   * powered/enabled from these lines, and after a service restart the
   * adapter is in whatever state the dying process's close left it
   * (hupcl drops the lines; one plain re-assert at open was not enough
   * to revive it on office-stim 2026-08-04 -- boot beeps were silent
   * until a manual re-open, which is exactly this cycle by accident). */
  {
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(info->midi_fd, TIOCMBIC, &lines);
    usleep(50000);
    ioctl(info->midi_fd, TIOCMBIS, &lines);
    usleep(50000);
  }
#endif

  /* Set or add hardware mode */
  info->mode |= SOUND_MODE_HARDWARE;

  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));

  return TCL_OK;
}

static int sound_reset_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  snd_reset(info);
  return TCL_OK;
}

static int sound_program_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int program, bank, ch_set;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "program bank channel_set");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &program) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &bank) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[3], &ch_set) != TCL_OK)
    return TCL_ERROR;

  snd_program(info, program, bank, ch_set);

  return TCL_OK;
}

static int sound_setfx_command (ClientData data, Tcl_Interp *interp,
				      int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int effect, channel;
  char program, bank, ch_set;

  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "effect channel");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &effect) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &channel) != TCL_OK)
    return TCL_ERROR;

  program = effect;
  bank = 0;
  ch_set = (2 << 4) | channel;

  snd_program(info, program, bank, ch_set);

  return TCL_OK;
}

static int sound_setdrum_command (ClientData data, Tcl_Interp *interp,
				      int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int drum, channel;
  char program, bank, ch_set;

  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "drum channel");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &drum) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &channel) != TCL_OK)
    return TCL_ERROR;

  program = drum;
  ch_set = (1 << 4) | channel;
  bank = 0;

  snd_program(info, program, bank, ch_set);

  return TCL_OK;
}

static int sound_setvoice_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int program, bank, ch_set;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "program bank channel");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &program) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &bank) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[3], &ch_set) != TCL_OK)
    return TCL_ERROR;

  snd_program(info, program, bank, ch_set);

  return TCL_OK;
}

static int sound_volume_command (ClientData data, Tcl_Interp *interp,
			   int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int volume, channel;
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "volume channel");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &volume) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &channel) != TCL_OK)
    return TCL_ERROR;

  snd_volume(info, volume, channel);

  return TCL_OK;
}

static int sound_gain_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  double level;
  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "level(0.0-1.0)");
    return TCL_ERROR;
  }

  if (Tcl_GetDoubleFromObj(interp, objv[1], &level) != TCL_OK)
    return TCL_ERROR;

  snd_master_gain(info, level);

  return TCL_OK;
}

static int sound_play_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int channel, pitch, duration_ms;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "channel pitch duration_ms");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &channel) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &pitch) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[3], &duration_ms) != TCL_OK)
    return TCL_ERROR;

  snd_on(info, channel, pitch);
  schedule_off(info, channel, pitch, duration_ms);

  return TCL_OK;
}

static void cleanup_fluidsynth(sound_info_t *info)
{
  retire_synth(info);
}

static int sound_list_alsa_devices_command(ClientData data, Tcl_Interp *interp,
                                           int objc, Tcl_Obj *objv[])
{
#if defined(__APPLE__) || defined(WIN32)
  Tcl_SetResult(interp, "ALSA device enumeration not available on macOS", TCL_STATIC);
  return TCL_ERROR;
#else
  void **hints, **n;
  Tcl_Obj *result_list = Tcl_NewListObj(0, NULL);

  // Get all PCM devices using ALSA hints
  if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
    Tcl_SetResult(interp, "Failed to get ALSA device hints", TCL_STATIC);
    return TCL_ERROR;
  }

  n = hints;
  while (*n != NULL) {
    char *name = snd_device_name_get_hint(*n, "NAME");
    char *desc = snd_device_name_get_hint(*n, "DESC");
    char *ioid = snd_device_name_get_hint(*n, "IOID");

    // Filter to only useful devices:
    // - Skip null, rate converters, and special plugins
    // - Skip surround configurations (keep stereo/front only)
    // - Keep default, plughw, dmix, sysdefault, and hw devices
    int include = 0;
    if (name && (!ioid || strcmp(ioid, "Input") != 0)) {
      if (strcmp(name, "default") == 0 ||
          strncmp(name, "plughw:", 7) == 0 ||
          strncmp(name, "dmix:", 5) == 0 ||
          strncmp(name, "sysdefault:", 11) == 0 ||
          strncmp(name, "hw:", 3) == 0) {
        include = 1;
      }
    }

    if (include) {
      Tcl_Obj *device_dict = Tcl_NewDictObj();

      Tcl_DictObjPut(interp, device_dict,
                     Tcl_NewStringObj("name", -1),
                     Tcl_NewStringObj(name, -1));

      if (desc) {
        // Replace newlines with spaces and trim
        char *desc_clean = strdup(desc);
        for (char *p = desc_clean; *p; p++) {
          if (*p == '\n') *p = ' ';
        }
        Tcl_DictObjPut(interp, device_dict,
                       Tcl_NewStringObj("description", -1),
                       Tcl_NewStringObj(desc_clean, -1));
        free(desc_clean);
      } else {
        Tcl_DictObjPut(interp, device_dict,
                       Tcl_NewStringObj("description", -1),
                       Tcl_NewStringObj("", -1));
      }

      Tcl_ListObjAppendElement(interp, result_list, device_dict);
    }

    if (name) free(name);
    if (desc) free(desc);
    if (ioid) free(ioid);
    n++;
  }

  snd_device_name_free_hint(hints);

  Tcl_SetObjResult(interp, result_list);
  return TCL_OK;
#endif
}

static int sound_init_fluidsynth_command(ClientData data, Tcl_Interp *interp,
                                         int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  const char *device_pattern = NULL;
  char err[256];

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "soundfont_path ?device_pattern?");
    return TCL_ERROR;
  }
  if (objc >= 3) device_pattern = Tcl_GetString(objv[2]);

  /* Clean up existing FluidSynth instance if any */
  cleanup_fluidsynth(info);

  info->settings = new_fluid_settings();
  if (!info->settings) {
    Tcl_SetResult(interp, "Failed to create FluidSynth settings", TCL_STATIC);
    return TCL_ERROR;
  }

  /* No audio driver: the miniaudio callback pulls samples via
   * fluid_synth_write_float().  Reverb/chorus off: task signals don't
   * need them and they dominate synth CPU on small boards. */
  fluid_settings_setnum(info->settings, "synth.sample-rate",
                        (double) AO_SAMPLE_RATE);
  fluid_settings_setint(info->settings, "synth.reverb.active", 0);
  fluid_settings_setint(info->settings, "synth.chorus.active", 0);
  fluid_settings_setnum(info->settings, "synth.gain", SND_FLUID_FULL_GAIN);

  info->synth = new_fluid_synth(info->settings);
  if (!info->synth) {
    Tcl_SetResult(interp, "Failed to create FluidSynth synth", TCL_STATIC);
    delete_fluid_settings(info->settings);
    info->settings = NULL;
    return TCL_ERROR;
  }

  if (fluid_synth_sfload(info->synth, Tcl_GetString(objv[1]), 1) ==
      FLUID_FAILED) {
    Tcl_AppendResult(interp, "Failed to load SoundFont: ",
                     Tcl_GetString(objv[1]), NULL);
    delete_fluid_synth(info->synth);
    delete_fluid_settings(info->settings);
    info->synth = NULL;
    info->settings = NULL;
    return TCL_ERROR;
  }

  if (audio_out_ensure(info, device_pattern, err, sizeof(err)) != 0) {
    delete_fluid_synth(info->synth);
    delete_fluid_settings(info->settings);
    info->synth = NULL;
    info->settings = NULL;
    Tcl_AppendResult(interp, err, NULL);
    return TCL_ERROR;
  }

  atomic_store_explicit(&info->render_synth, info->synth,
                        memory_order_release);
  info->mode |= SOUND_MODE_SOFTWARE;

  return TCL_OK;
}

/*************************  wav commands  ********************************/

/* install decoded PCM into the wav table under `name` (replacing any
 * same-named entry), publish sound/wav/loaded, and leave the duration in
 * ms as the interp result. Takes ownership of pcm (freed on error). */
static int wav_install(sound_info_t *info, Tcl_Interp *interp,
                       const char *cmdname, const char *name,
                       float *pcm, ma_uint64 nframes)
{
  if (strlen(name) >= WAV_NAME_MAX) {
    free(pcm);
    Tcl_AppendResult(interp, cmdname, ": name too long", NULL);
    return TCL_ERROR;
  }

  /* replace an existing entry of the same name, or take a free slot */
  int idx = wav_find(info, name);
  if (idx >= 0) {
    wav_stop_voices(info, idx);
    if (wav_drain(info, idx, 250) != 0) {
      free(pcm);
      Tcl_AppendResult(interp, cmdname,
                       ": voices still active on \"", name, "\"", NULL);
      return TCL_ERROR;
    }
    free(info->wavs[idx].pcm);
  } else {
    for (int i = 0; i < MAX_WAVS; i++)
      if (!info->wavs[i].used) { idx = i; break; }
    if (idx < 0) {
      free(pcm);
      Tcl_AppendResult(interp, cmdname, ": wav table full", NULL);
      return TCL_ERROR;
    }
  }

  wav_entry_t *w = &info->wavs[idx];
  strncpy(w->name, name, WAV_NAME_MAX - 1);
  w->name[WAV_NAME_MAX - 1] = '\0';
  w->pcm = pcm;
  w->nframes = nframes;
  w->used = 1;

  int duration_ms = (int) ((nframes * 1000ULL) / AO_SAMPLE_RATE);

  char loaded[WAV_NAME_MAX + 32];
  snprintf(loaded, sizeof(loaded), "%s %d", w->name, duration_ms);
  publish_string(info, PT_WAV_LOADED, loaded);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(duration_ms));
  return TCL_OK;
}

static int wav_load_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  char err[512];

  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "name path");
    return TCL_ERROR;
  }
  const char *name = Tcl_GetString(objv[1]);
  const char *path = Tcl_GetString(objv[2]);

  ma_uint64 nframes = 0;
  float *pcm = wav_decode_file(path, &nframes, err, sizeof(err));
  if (!pcm) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": ", err, NULL);
    return TCL_ERROR;
  }

  return wav_install(info, interp, Tcl_GetString(objv[0]), name,
                     pcm, nframes);
}

/* wavLoadData name bytes -- like wavLoad, but the sound file image
 * (wav/flac/mp3 bytes, e.g. from the dlsh wav package's wav::render or a
 * [read] of a file) arrives as a Tcl byte array instead of a path, so
 * procedurally generated stimuli never touch the filesystem. */
static int wav_load_data_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  char err[512];

  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "name sound_file_bytes");
    return TCL_ERROR;
  }
  const char *name = Tcl_GetString(objv[1]);

  Tcl_Size nbytes = 0;
  unsigned char *bytes = Tcl_GetByteArrayFromObj(objv[2], &nbytes);
  if (!bytes || nbytes == 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": empty sound data", NULL);
    return TCL_ERROR;
  }

  ma_uint64 nframes = 0;
  float *pcm = wav_decode_memory(bytes, (size_t) nbytes, &nframes,
                                 err, sizeof(err));
  if (!pcm) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": ", err, NULL);
    return TCL_ERROR;
  }

  return wav_install(info, interp, Tcl_GetString(objv[0]), name,
                     pcm, nframes);
}

static int wav_play_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  char err[256];
  double gain = 1.0;
  int loop = 0;

  if (objc < 2 || objc > 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "name ?gain? ?loop?");
    return TCL_ERROR;
  }
  const char *name = Tcl_GetString(objv[1]);
  if (objc >= 3 && Tcl_GetDoubleFromObj(interp, objv[2], &gain) != TCL_OK)
    return TCL_ERROR;
  if (objc >= 4 && Tcl_GetIntFromObj(interp, objv[3], &loop) != TCL_OK)
    return TCL_ERROR;

  int idx = wav_find(info, name);
  if (idx < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": no wav named \"", name, "\" (use wavLoad)", NULL);
    return TCL_ERROR;
  }

  /* device starts on demand so wav-only rigs never touch fluidsynth */
  if (audio_out_ensure(info, NULL, err, sizeof(err)) != 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": ", err, NULL);
    return TCL_ERROR;
  }

  for (int i = 0; i < MAX_WAV_VOICES; i++) {
    wav_voice_t *v = &info->voices[i];
    if (atomic_load_explicit(&v->state, memory_order_acquire)
        == WAV_VOICE_FREE) {
      v->wav_index = idx;
      strncpy(v->name, info->wavs[idx].name, WAV_NAME_MAX - 1);
      v->name[WAV_NAME_MAX - 1] = '\0';
      v->pcm = info->wavs[idx].pcm;
      v->nframes = info->wavs[idx].nframes;
      v->pos = 0;
      v->gain = (float) gain;
      v->loop = loop;
      atomic_store_explicit(&v->state, WAV_VOICE_START, memory_order_release);
      return TCL_OK;
    }
  }

  Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                   ": no free voices", NULL);
  return TCL_ERROR;
}

static int wav_stop_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  if (objc > 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?name?");
    return TCL_ERROR;
  }

  if (objc == 2) {
    const char *name = Tcl_GetString(objv[1]);
    int idx = wav_find(info, name);
    if (idx < 0) {
      Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                       ": no wav named \"", name, "\"", NULL);
      return TCL_ERROR;
    }
    wav_stop_voices(info, idx);
  } else {
    wav_stop_voices(info, -1);
  }
  return TCL_OK;
}

static int wav_unload_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "name");
    return TCL_ERROR;
  }
  const char *name = Tcl_GetString(objv[1]);
  int idx = wav_find(info, name);
  if (idx < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": no wav named \"", name, "\"", NULL);
    return TCL_ERROR;
  }

  wav_stop_voices(info, idx);
  if (wav_drain(info, idx, 250) != 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": voices still active on \"", name, "\"", NULL);
    return TCL_ERROR;
  }

  free(info->wavs[idx].pcm);
  memset(&info->wavs[idx], 0, sizeof(wav_entry_t));
  return TCL_OK;
}

static int wav_list_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  Tcl_Obj *result = Tcl_NewListObj(0, NULL);

  for (int i = 0; i < MAX_WAVS; i++)
    if (info->wavs[i].used)
      Tcl_ListObjAppendElement(interp, result,
                               Tcl_NewStringObj(info->wavs[i].name, -1));
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

static int wav_info_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "name");
    return TCL_ERROR;
  }
  const char *name = Tcl_GetString(objv[1]);
  int idx = wav_find(info, name);
  if (idx < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": no wav named \"", name, "\"", NULL);
    return TCL_ERROR;
  }

  wav_entry_t *w = &info->wavs[idx];
  Tcl_Obj *d = Tcl_NewDictObj();
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("frames", -1),
                 Tcl_NewWideIntObj((Tcl_WideInt) w->nframes));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("duration_ms", -1),
                 Tcl_NewIntObj((int)((w->nframes * 1000ULL) / AO_SAMPLE_RATE)));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("active_voices", -1),
                 Tcl_NewIntObj(wav_voices_active(info, idx)));
  Tcl_SetObjResult(interp, d);
  return TCL_OK;
}

/*************************  audio commands  ******************************/

static int audio_init_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  char err[256];
  const char *pattern = NULL;

  if (objc > 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?device_pattern?");
    return TCL_ERROR;
  }
  if (objc == 2) pattern = Tcl_GetString(objv[1]);

  if (audio_out_ensure(info, pattern, err, sizeof(err)) != 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": ", err, NULL);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewStringObj(info->ao_name, -1));
  return TCL_OK;
}

static int audio_info_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;

  int nwavs = 0;
  for (int i = 0; i < MAX_WAVS; i++) if (info->wavs[i].used) nwavs++;

  Tcl_Obj *d = Tcl_NewDictObj();
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("running", -1),
                 Tcl_NewIntObj(atomic_load(&info->ao_running) ? 1 : 0));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("device", -1),
                 Tcl_NewStringObj(info->ao_initialized ? info->ao_name : "", -1));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("sample_rate", -1),
                 Tcl_NewIntObj(AO_SAMPLE_RATE));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("period_frames", -1),
                 Tcl_NewIntObj(AO_PERIOD_FRAMES));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("periods", -1),
                 Tcl_NewIntObj(AO_PERIODS));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("synth", -1),
                 Tcl_NewIntObj(info->synth ? 1 : 0));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("mode", -1),
                 Tcl_NewIntObj((int) info->mode));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("wavs", -1),
                 Tcl_NewIntObj(nwavs));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("active_voices", -1),
                 Tcl_NewIntObj(wav_voices_active(info, -1)));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("master_gain", -1),
                 Tcl_NewDoubleObj((double)
                   atomic_load_explicit(&info->master_gain,
                                        memory_order_relaxed)));
  Tcl_SetObjResult(interp, d);
  return TCL_OK;
}

static int audio_devices_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
  ma_context context;
  if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
    Tcl_SetResult(interp, "failed to initialize audio context", TCL_STATIC);
    return TCL_ERROR;
  }

  Tcl_Obj *result = Tcl_NewListObj(0, NULL);
  ma_device_info *devices;
  ma_uint32 count;
  if (ma_context_get_devices(&context, &devices, &count,
                             NULL, NULL) == MA_SUCCESS) {
    for (ma_uint32 i = 0; i < count; i++)
      Tcl_ListObjAppendElement(interp, result,
                               Tcl_NewStringObj(devices[i].name, -1));
  }
  ma_context_uninit(&context);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_sound_Init) (Tcl_Interp *interp)
#else
  int Dserv_sound_Init(Tcl_Interp *interp)
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

  /* Allocate per-interpreter sound info */
  sound_info_t *info = (sound_info_t *) calloc(1, sizeof(sound_info_t));
  if (!info) {
    Tcl_SetResult(interp, "Failed to allocate sound_info_t", TCL_STATIC);
    return TCL_ERROR;
  }

  info->mode = SOUND_MODE_NONE;
  info->midi_fd = -1;
  atomic_store_explicit(&info->master_gain, 1.0f, memory_order_relaxed);

  /* Resolve the host datapoint API dynamically: inside dserv these all
   * resolve and wav onset/offset/loaded points are published; in a plain
   * tclsh (testing) they don't, and publishing is silently disabled. */
  info->api.get_from_interp = (tclserver_t *(*)(Tcl_Interp *))
    dlsym(RTLD_DEFAULT, "tclserver_get_from_interp");
  info->api.now = (uint64_t (*)(tclserver_t *))
    dlsym(RTLD_DEFAULT, "tclserver_now");
  info->api.set_point = (void (*)(tclserver_t *, ds_datapoint_t *))
    dlsym(RTLD_DEFAULT, "tclserver_set_point");
  info->api.new_point = (ds_datapoint_t *(*)(char *, uint64_t, ds_datatype_t,
                                             uint32_t, unsigned char *))
    dlsym(RTLD_DEFAULT, "dpoint_new");
  if (info->api.get_from_interp)
    info->tclserver = info->api.get_from_interp(interp);

  /* Hardware initialization */
  Tcl_CreateObjCommand(interp, "soundOpen",
		       (Tcl_ObjCmdProc *) sound_open_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);

  /* Software initialization */
  Tcl_CreateObjCommand(interp, "soundInitFluidSynth",
		       (Tcl_ObjCmdProc *) sound_init_fluidsynth_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);

  /* Common commands */
  Tcl_CreateObjCommand(interp, "soundReset",
		       (Tcl_ObjCmdProc *) sound_reset_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetFX",
		       (Tcl_ObjCmdProc *) sound_setfx_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetVoice",
		       (Tcl_ObjCmdProc *) sound_setvoice_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetDrum",
		       (Tcl_ObjCmdProc *) sound_setdrum_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetVolume",
		       (Tcl_ObjCmdProc *) sound_volume_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundVolume",
		       (Tcl_ObjCmdProc *) sound_volume_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundGain",
		       (Tcl_ObjCmdProc *) sound_gain_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundListAlsaDevices",
                       (Tcl_ObjCmdProc *) sound_list_alsa_devices_command,
                       (ClientData) info,
                       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundPlay",
		       (Tcl_ObjCmdProc *) sound_play_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);

  /* Wav stimulus commands */
  Tcl_CreateObjCommand(interp, "wavLoad",
                       (Tcl_ObjCmdProc *) wav_load_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavLoadData",
                       (Tcl_ObjCmdProc *) wav_load_data_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavPlay",
                       (Tcl_ObjCmdProc *) wav_play_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavStop",
                       (Tcl_ObjCmdProc *) wav_stop_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavUnload",
                       (Tcl_ObjCmdProc *) wav_unload_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavList",
                       (Tcl_ObjCmdProc *) wav_list_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "wavInfo",
                       (Tcl_ObjCmdProc *) wav_info_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);

  /* Audio output commands */
  Tcl_CreateObjCommand(interp, "audioInit",
                       (Tcl_ObjCmdProc *) audio_init_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "audioInfo",
                       (Tcl_ObjCmdProc *) audio_info_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "audioDevices",
                       (Tcl_ObjCmdProc *) audio_devices_command,
                       (ClientData) info, (Tcl_CmdDeleteProc *) NULL);

  return TCL_OK;
}
