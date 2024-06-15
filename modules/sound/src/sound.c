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
} queue;

queue* queueCreate();
void enqueue(queue* q, struct offinfo_s *offinfo);
struct offinfo_s *dequeue(queue* q);

queue* queueCreate() {
    queue *q = (queue*) malloc(sizeof(queue));
    q->front = 0;
    q->back = 0;

    q->mutex = sem_open ("qMutex", O_CREAT | O_EXCL, 0644, 1); 
    sem_unlink ("qMutex");      

    q->slots = sem_open ("qSlots", O_CREAT | O_EXCL, 0644, QUEUE_SIZE); 
    sem_unlink ("qSlots");      

    q->items = sem_open ("qItems", O_CREAT | O_EXCL, 0644, 0); 
    sem_unlink ("qItems");      
    
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

static int snd_on(int, char channel, char pitch);
static int snd_off(int, char channel, char pitch);
static int snd_control(int, char control, char data, char channel);
static int snd_program(int, char program, char bank, char ch_set);
static int snd_reset(int);
static int snd_volume(int, char volume, char channel);

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
  int midi_fd;
  queue* q;
} sound_info_t;

/* global to this module */
static sound_info_t g_soundInfo;


/**************************************************************
 *
 * FUNCTION
 *   sound_program
 *
 * DESCRIPTION
 *   Send a program change message to the midi driver
 * Note that the channel and set are packed into the
 * ch_set variable, since channels are only between 0-15
 * and there are only 3 sets (VOICES, DRUMS, SFX).
 *
 **************************************************************/

static int snd_program(int midi_fd, char program, char bank, char ch_set)
{
  char set, channel;
  char cmd[8];
  
  channel = ch_set & 0x0F;	/* Low nibble  */
  set = (ch_set & 0xF0) >> 4; /* High nibble */
  
  if (set > sizeof(Sets)) return 0;
  
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
  
  write(midi_fd, cmd, sizeof(cmd));
  
  /* Now set the volume to the middle */
  snd_control(midi_fd, MIDI_CTRL_VOLUME, 64, channel);
  
  return 1;
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

static int snd_volume(int midi_fd, char volume, char channel)
{
  snd_control(midi_fd, MIDI_CTRL_VOLUME, volume, channel);
  return 1;
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

static int snd_control(int midi_fd, char control, char data, char channel)
{
  char cmd[3];
  
  cmd[0] = 0xb0 | channel;
  cmd[1] = control;
  cmd[2] = data;
  
  write(midi_fd, cmd, sizeof(cmd));
  return 1;
}

static int snd_reset(int midi_fd)
{
  static char xg_on[] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00,
    0x7e, 0x00, 0xf7 };
#if 0
  static char velocity_sensitivity[] = {
    0xf0, 0x43, 0x10, 0x4c,
    0x08, 0x01, 0x0c, 0xf5, 0xf7 };
#endif
  static char master_volume[] = { 0xf0, 0x7f, 0x7f, 0x04, 0x01,
    0x7f, 0x7f,
    0xf7 };
  
  write(midi_fd, xg_on, sizeof(xg_on));

  /* according to the MU15 docs, the xg_on command takes approx 50ms */
  usleep(50000);
#if 0  
  write(midi_fd, velocity_sensitivity, sizeof(velocity_sensitivity));
#endif
  
  write(midi_fd, master_volume, sizeof(master_volume));
  return 1;
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

static int snd_on(int midi_fd, char channel, char pitch)
{
  int i;
  static char vel = 127;
  char cmd[3];

  /* Do the actual note on */
  cmd[0] = 0x90 | channel;
  cmd[1] = pitch;
  cmd[2] = vel;
  write(midi_fd, cmd, 3);
  
  /* Turn on the sustain event */
  snd_control(midi_fd, MIDI_CTRL_SUSTENTO, MIDI_ON, channel);

  return i;
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
static int snd_off(int midi_fd, char channel, char pitch)
{
  char cmd[3];
  static char vel = 127;
  
  /* Turn off the sustain event */
  snd_control(midi_fd, MIDI_CTRL_SUSTENTO, MIDI_OFF, channel);
  
  /* Do the actual note off */
  cmd[0] = 0x80 | channel;
  cmd[1] = pitch;
  cmd[2] = vel;
  write(midi_fd, cmd, 3);
  
  //  printf("sound off %d %d %d\n", channel, pitch, vel);
  return 0;
}    


int serveOffRequest(offinfo_t *off)
{
  struct timespec rqtp = { off->ms/1000, (off->ms%1000)*1000000 };
  nanosleep(&rqtp, NULL);
  snd_off(off->info->midi_fd, off->channel, off->pitch);  

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
  configure_serial_port(info->midi_fd);
  return TCL_OK;
}

static int sound_reset_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[])
{
  sound_info_t *info = (sound_info_t *) data;
  if (info->midi_fd >= 0)
    snd_reset(info->midi_fd);
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
  if (info->midi_fd >= 0) 
    snd_program(info->midi_fd, program, bank, ch_set);

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
  
  if (info->midi_fd >= 0) 
    snd_program(info->midi_fd, program, bank, ch_set);

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
  
  if (info->midi_fd >= 0) 
    snd_program(info->midi_fd, program, bank, ch_set);

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
  if (info->midi_fd >= 0) 
    snd_program(info->midi_fd, program, bank, ch_set);

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
  
  if (info->midi_fd >= 0)
    snd_volume(info->midi_fd, volume, channel);
  
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
  if (info->midi_fd >= 0)
    snd_on(info->midi_fd, channel, pitch);
  
  enqueue(info->q, request);
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
      Tcl_InitStubs(interp, "8.6", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }
  const int nworkers = 5;
  
  g_soundInfo.q = queueCreate();
  g_soundInfo.midi_fd = -1;
  
  /* setup workers */
  pthread_t w;
  for (int i = 0; i < nworkers; i++) {
    pthread_create(&w, NULL, workerThread, &g_soundInfo);
  }
  
  Tcl_CreateObjCommand(interp, "soundOpen",
		       (Tcl_ObjCmdProc *) sound_open_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundReset",
		       (Tcl_ObjCmdProc *) sound_reset_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetFX",
		       (Tcl_ObjCmdProc *) sound_setfx_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetVoice",
		       (Tcl_ObjCmdProc *) sound_setvoice_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetDrum",
		       (Tcl_ObjCmdProc *) sound_setdrum_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundSetVolume",
		       (Tcl_ObjCmdProc *) sound_volume_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundVolume",
		       (Tcl_ObjCmdProc *) sound_volume_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "soundPlay",
		       (Tcl_ObjCmdProc *) sound_play_command,
		       (ClientData) &g_soundInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}



