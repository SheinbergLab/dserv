/*
 * NAME
 *   sound.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 06/24
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <termios.h>
#include <pthread.h>
#include <semaphore.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

/* FluidSynth for software synthesis */
#include <fluidsynth.h>

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
/***                     queues for sound off events                   ***/
/*************************************************************************/

/*
  semaphore example:
  http://www2.lawrence.edu/fast/GREGGJ/CMSC480/net/workerThreads.html
*/
#define QUEUE_SIZE 16

struct offinfo_s;

typedef struct {
  struct offinfo_s *d[QUEUE_SIZE];
  int front;
  int back;
  sem_t *mutex;
  sem_t *slots;
  sem_t *items;
#ifndef __APPLE__
  sem_t unnamed_mutex;
  sem_t unnamed_slots;
  sem_t unnamed_items;
#endif
} queue;

queue* queueCreate();
void enqueue(queue* q, struct offinfo_s *offinfo);
struct offinfo_s *dequeue(queue* q);

queue* queueCreate() {
    queue *q = (queue*) malloc(sizeof(queue));
    q->front = 0;
    q->back = 0;

#ifdef __APPLE__
    q->mutex = sem_open ("qMutex", O_CREAT | O_EXCL, 0644, 1); 
    sem_unlink ("qMutex");      

    q->slots = sem_open ("qSlots", O_CREAT | O_EXCL, 0644, QUEUE_SIZE); 
    sem_unlink ("qSlots");      

    q->items = sem_open ("qItems", O_CREAT | O_EXCL, 0644, 0); 
    sem_unlink ("qItems");      
#else
    q->mutex = &q->unnamed_mutex;
    sem_init(q->mutex, 0, 1);

    q->slots = &q->unnamed_slots;
    sem_init(q->slots, 0, QUEUE_SIZE);

    q->items = &q->unnamed_items;
    sem_init(q->items, 0, 0);
#endif
    return q;
}

void enqueue(queue* q, struct offinfo_s *offinfo) {
    sem_wait(q->slots);
    sem_wait(q->mutex);
    q->d[q->back] = offinfo;
    q->back = (q->back+1)%QUEUE_SIZE;
    sem_post(q->mutex);
    sem_post(q->items);
}

struct offinfo_s *dequeue(queue* q) {
  struct offinfo_s *offinfo;
  sem_wait(q->items);
  sem_wait(q->mutex);
  offinfo = q->d[q->front];
  q->front = (q->front+1)%QUEUE_SIZE;
  sem_post(q->mutex);
  sem_post(q->slots);
  return offinfo;
}

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

/*
 * Different write messages supported are identified
 * by their length
 */

#define WRITE_RESET        (1)    /* single byte                  */	
#define WRITE_VOLUME       (2)    /* channel / volume             */
#define WRITE_PROGRAM      (3)    /* program, bank, set+channel   */
#define WRITE_SOUNDON      (4)    /* sizeof(short)+2*sizeof(char) */

/* Set array for program change message */
static char Sets[] = { MIDI_VOICES, MIDI_DRUMS, MIDI_SFX };

#define WBUF90          (WBUF*9/10)     //flush trip point
#define NOTE_OFF        '\x80'          //MIDI channel command
#define NOTE_ON         '\x90'          //MIDI channel command
#define CHANNEL_CONTROL '\xb0'          //MIDI channel command
#define PROGRAM_CHANGE  '\xc0'          //MIDI channel command
#define PITCH_BEND      '\xe0'          //MIDI channel command

struct sound_info_s;

typedef struct offinfo_s {
  struct sound_info_s *info;
  int ms;			/* when to turn off    */
  char channel;			/* channel to turn off */
  char pitch;			/* pitch to turn off   */
} offinfo_t;

typedef struct sound_info_s
{
  sound_mode_t mode;

  /* Hardware mode (MIDI over serial) */  
  int midi_fd;

  /* Software mode (FluidSynth) */
  fluid_settings_t *settings;   /* FluidSynth settings */
  fluid_synth_t *synth;         /* FluidSynth synthesizer */
  fluid_audio_driver_t *adriver; /* FluidSynth audio driver */

  queue* q;
} sound_info_t;

static int snd_on(sound_info_t *, char channel, char pitch);
static int snd_off(sound_info_t *, char channel, char pitch);
static int snd_control(sound_info_t *, char control, char data, char channel);
static int snd_program(sound_info_t *, char program, char bank, char ch_set);
static int snd_reset(sound_info_t *);
static int snd_volume(sound_info_t *, char volume, char channel);

/**************************************************************
 *
 * FUNCTION
 *   sound_program
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
  
  channel = ch_set & 0x0F;	/* Low nibble  */
  set = (ch_set & 0xF0) >> 4; /* High nibble */
  
  if (set > sizeof(Sets)) return 0;
  
  if (info->mode & SOUND_MODE_HARDWARE) {
    /* Hardware mode - send MIDI over serial */
    char cmd[8];
    int n = 0;
    
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
    
    return n;
    
  } else if (info->mode & SOUND_MODE_SOFTWARE) {
    /* Software mode - use FluidSynth */
    if (!info->synth) return 0;
    
    /* Bank select MSB (CC 0) */
    fluid_synth_cc(info->synth, channel, 0, Sets[(int)set]);
    
    /* Bank select LSB (CC 32) */
    fluid_synth_cc(info->synth, channel, 32, bank);
    
    /* Program change */
    fluid_synth_program_change(info->synth, channel, program - 1);
    
    /* Set volume to middle */
    fluid_synth_cc(info->synth, channel, MIDI_CTRL_VOLUME, 64);
    
    return 1;
  }
  
  return 0;
}

/**************************************************************
 *
 * FUNCTION
 *   sound_volume
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
 *   sound_control
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
    /* Software mode - use FluidSynth */
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
    
    if (info->midi_fd >= 0)
      n = write(info->midi_fd, master_volume, sizeof(master_volume));
  }
  
  if (info->mode & SOUND_MODE_SOFTWARE) {
    /* Software mode - FluidSynth system reset */
    if (info->synth) {
      fluid_synth_system_reset(info->synth);
      n = 1;
    }
  }
  
  return n;
}

/**************************************************************
 * FUNCTION
 *   sound_on
 *
 * DESCRIPTION
 *   Turn on the sound for the specified channel
 *   and schedule it to go off time ms in the future
 *
 * NOTE
 *   We use a free OFF_INFO structure to pass clientdata to the
 *   scheduled off function without fear that some variable
 *   will change upon subsequent calls.
 **************************************************************/

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
    /* Software mode - FluidSynth note-on */
    if (info->synth)
      n = fluid_synth_noteon(info->synth, channel, pitch, vel);
  }
  
  return n;
}

/*
 * FUNCTION
 *   sound_off
 *
 * DESCRIPTION
 *   turn off the sound for the channel(s) pointed to
 *   by clientdata (which was supplied in the sound_on
 *   function
 */
/**************************************************************
 * FUNCTION
 *   snd_off
 *
 * DESCRIPTION
 *   Turn off the sound for the specified channel
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
    /* Software mode - FluidSynth note-off */
    if (info->synth)
      n = fluid_synth_noteoff(info->synth, channel, pitch);
  }
  
  return n;
}

static int serveOffRequest(offinfo_t *off)
{
  sound_info_t *info = off->info;
  
  /* sleep for the note duration */
  usleep(off->ms * 1000);
  
  /* send note-off */
  snd_off(info, off->channel, off->pitch);
  
  /* free the request */
  free(off);
  return 0;
}

void* workerThread(void *arg) {
  sound_info_t *info = (sound_info_t *) arg;
  
  while(1) {
    offinfo_t *req = dequeue(info->q);
    serveOffRequest(req);
  }
  return NULL;
}

static offinfo_t *new_offinfo(sound_info_t *info,
			      int channel, int pitch, int ms)
{
  offinfo_t *request = (offinfo_t *) malloc(sizeof(offinfo_t));
  request->info = info;
  request->ms = ms;		/* when to turn off    */
  request->channel = channel;	/* channel to turn off */
  request->pitch = pitch;	/* pitch to turn off   */
  return request;
}


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

  offinfo_t *request = new_offinfo(info, channel, pitch, duration_ms);
  snd_on(info, channel, pitch);
  
  enqueue(info->q, request);
  return TCL_OK;
}

static void cleanup_fluidsynth(sound_info_t *info)
{
  if (info->adriver) {
    delete_fluid_audio_driver(info->adriver);
    info->adriver = NULL;
  }
  
  if (info->synth) {
    delete_fluid_synth(info->synth);
    info->synth = NULL;
  }
  
  if (info->settings) {
    delete_fluid_settings(info->settings);
    info->settings = NULL;
  }
}


static const char* find_working_alsa_device(fluid_settings_t *settings) {
  const char* devices_to_try[] = {
    "default",           // Try system default first
    "plughw:0,0",       // Then first hardware device with conversion
    "sysdefault",       // System default without card specification
    "hw:0,0",           // Direct hardware access as last resort
    NULL
  };
  
  // Save the current settings
  fluid_settings_t *test_settings = new_fluid_settings();
  if (!test_settings) {
    return "default";
  }
  
  // Copy relevant settings
  fluid_settings_setstr(test_settings, "audio.driver", "alsa");
  fluid_settings_setnum(test_settings, "synth.sample-rate", 44100.0);
  fluid_settings_setint(test_settings, "audio.period-size", 256);
  fluid_settings_setint(test_settings, "audio.periods", 2);
  
  for (int i = 0; devices_to_try[i] != NULL; i++) {
    fluid_settings_setstr(test_settings, "audio.alsa.device", devices_to_try[i]);
    
    // Try to create a temporary synth and audio driver to test
    fluid_synth_t *test_synth = new_fluid_synth(test_settings);
    if (test_synth) {
      fluid_audio_driver_t *test_driver = new_fluid_audio_driver(test_settings, test_synth);
      if (test_driver) {
        // Success! Clean up and return this device
        delete_fluid_audio_driver(test_driver);
        delete_fluid_synth(test_synth);
        delete_fluid_settings(test_settings);
        return devices_to_try[i];
      }
      delete_fluid_synth(test_synth);
    }
  }
  
  delete_fluid_settings(test_settings);
  return "default"; // Fall back to default if nothing works
}

static int sound_init_fluidsynth_command(ClientData data, Tcl_Interp *interp,
                                         int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  const char *alsa_device = NULL;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "soundfont_path ?alsa_device?");
    return TCL_ERROR;
  }
  
  /* Clean up existing FluidSynth instance if any */
  cleanup_fluidsynth(info);
  
  /* Create settings */
  info->settings = new_fluid_settings();
  if (!info->settings) {
    Tcl_SetResult(interp, "Failed to create FluidSynth settings", TCL_STATIC);
    return TCL_ERROR;
  }
  
  /* Configure audio driver based on platform */
#ifdef __APPLE__
  fluid_settings_setstr(info->settings, "audio.driver", "coreaudio");
#else
  fluid_settings_setstr(info->settings, "audio.driver", "alsa");
  
  // Determine ALSA device
  if (objc >= 3) {
    // User specified device explicitly
    alsa_device = Tcl_GetString(objv[2]);
  } else {
    // Auto-detect working device
    alsa_device = find_working_alsa_device(info->settings);
  }
  
  fluid_settings_setstr(info->settings, "audio.alsa.device", alsa_device);
#endif
  
  /* Configure audio quality/latency - use correct parameter names */
  fluid_settings_setnum(info->settings, "synth.sample-rate", 44100.0);
  fluid_settings_setint(info->settings, "audio.period-size", 256);
  fluid_settings_setint(info->settings, "audio.periods", 2);
  
  /* Create synthesizer */
  info->synth = new_fluid_synth(info->settings);
  if (!info->synth) {
    Tcl_SetResult(interp, "Failed to create FluidSynth synth", TCL_STATIC);
    cleanup_fluidsynth(info);
    return TCL_ERROR;
  }
  
  /* Load SoundFont */
  if (fluid_synth_sfload(info->synth, Tcl_GetString(objv[1]), 1) ==
      FLUID_FAILED) {
    Tcl_AppendResult(interp, "Failed to load SoundFont: ",
                     Tcl_GetString(objv[1]), NULL);
    cleanup_fluidsynth(info);
    return TCL_ERROR;
  }
  
  /* Create audio driver (starts audio output) */
  info->adriver = new_fluid_audio_driver(info->settings, info->synth);
  if (!info->adriver) {
    Tcl_SetResult(interp, "Failed to create FluidSynth audio driver",
		  TCL_STATIC);
    cleanup_fluidsynth(info);
    return TCL_ERROR;
  }
  
  /* Set or add software mode */
  info->mode |= SOUND_MODE_SOFTWARE;
  
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
  
  /* Initialize the structure */
  info->mode = SOUND_MODE_NONE;
  info->midi_fd = -1;
  info->settings = NULL;
  info->synth = NULL;
  info->adriver = NULL;
  info->q = queueCreate();
  
  const int nworkers = 5;
  
  /* setup workers */
  pthread_t w;
  for (int i = 0; i < nworkers; i++) {
    pthread_create(&w, NULL, workerThread, info);
  }
  
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
  Tcl_CreateObjCommand(interp, "soundPlay",
		       (Tcl_ObjCmdProc *) sound_play_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}




