/*
 * NAME
 *   ina226.c
 *
 * DESCRIPTION
 *   This module acquires battery readings from ina226 chips and sends the acquired
 *  values to dserv.
 *
 * DPOINTS
 *   float    ${PREFIX}/12v-v
 *   float    ${PREFIX}/12v-a
 *   float    ${PREFIX}/24v-v
 *   float    ${PREFIX}/24v-a
 *
 * AUTHOR
 *   DLS, 06/24-08/24
 *
 */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>			
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <pthread.h>

#ifdef __linux__
#include <sys/timerfd.h>
#include <linux/i2c-dev.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

int i2cReadRegister(int i2cfd, uint8_t slaveaddr, uint8_t reg,
		    uint8_t *buf, int len)
{
#ifdef __linux__
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data msgget[1];

  msgs[0].addr = slaveaddr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[0].addr = slaveaddr;
  msgs[0].flags = I2C_M_RD;
  msgs[0].len = len;
  msgs[0].buf = buf;

  msgset[0].msgs = msgs;
  msgset[0].nmsgs = 2;
  
  if (ioctl(i2cfd, I2C_RDWR, &msgset) < 0) {
    return -1;
  }
#endif
  return 0;
}

int i2cWriteRegister(int i2cfd, uint8_t slaveaddr, uint8_t reg,
		     uint8_t *buf, int len)
{
#ifdef __linux__
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data msgget[1];

  msgs[0].addr = slaveaddr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[0].addr = slaveaddr;
  msgs[0].flags = 0;
  msgs[0].len = len;
  msgs[0].buf = buf;

  msgset[0].msgs = msgs;
  msgset[0].nmsgs = 2;
  
  if (ioctl(i2cfd, I2C_RDWR, &msgset) < 0) {
    return -1;
  }
#endif
  return 0;
}

int i2cWriteWord16(int i2cfd, uint8_t slaveaddr, uint8_t reg,
		 uint16_t data)
{
  uint8_t buf[2];
  buf[0] = (data & 0xFF00) >> 8;
  buf[1] = (data & 0xFF);
  return i2cWriteRegister(i2cfd, slaveaddr, reg, buf, 2);
}

uint16_t i2cReadWord16(int i2cfd, uint8_t slaveaddr, uint8_t reg)
{
  uint8_t buf[2];
  i2cReadRegister(i2cfd, slaveaddr, reg, buf, 2);
  return ((uint16_t) (buf[0] << 8 | buf[1]));
}
			
#define MAX_INA226_CONFIGS 10
			
typedef struct ina226_config_s {
  atomic_int active;

  // file descriptor for I2C read/write
  int fd;

  // server for storing points to
  tclserver_t *tclserver;

  float shunt_ohms;
  float max_expected_amps;
  uint8_t current_lsb;
  uint16_t calibration_value;
  uint8_t config_bytes[2];
  
  // I2C addresses for INA226 devices
  uint8_t address;

  char name[128];
} ina226_config_t;

typedef struct ina226_info_s
{
  tclserver_t *tclserver;

  // file descriptor for I2C read/write
  int fd;
  
  // I2C bus number
  int bus_number;

  // slots for up to MAX_INS226 configurations
  ina226_config_t configs[MAX_INA226_CONFIGS];

  int timer_fd;			/* timerfd_XXX fd           */
#ifdef __linux__
  pthread_t timer_thread_id;
#endif
  int interval_sec;		/* how often to acq (sec)   */
} ina226_info_t;

/* global to this module */
static ina226_info_t g_ina226Info;

static int ina226_trigger(ina226_config_t *config)
{  
  // Trigger a single-shot conversion by writing to the configuration register
  return i2cWriteRegister(config->fd, config->address, 0x00, config->config_bytes, 2);
}

static int ina226_conversion_complete(ina226_config_t *config)
{
  uint16_t mask_enable_reg = i2cReadWord16(config->fd, config->address, 0x06);
  return (mask_enable_reg & 0x0008);
}

static int ina226_initialize(ina226_info_t *ina226info,
			     uint8_t address, char *name, char *prefix)
{
  ina226_config_t *config = NULL;
  if (ina226info->fd < 0) return -1;

  int slot;
  
  for (slot = 0; slot < MAX_INA226_CONFIGS; slot++) {
    if (!ina226info->configs[slot].active) {
      config = &ina226info->configs[slot];
      break;
    }
  }
  
  /* no slots left */
  if (slot == MAX_INA226_CONFIGS) return -1;

  /* copy fd so we can access from config */
  config->fd = ina226info->fd;
  
  /* copy tclserver so we can use to store datapoints */
  config->tclserver = ina226info->tclserver;
  
  const char *DEFAULT_DPOINT_PREFIX = "system/battery";
  if (!prefix) prefix = (char *) DEFAULT_DPOINT_PREFIX;
  
  config->shunt_ohms = 0.1;	   /* 100 milliohms              */
  config->max_expected_amps = 2; /* Maximum expected current   */
  config->current_lsb = config->max_expected_amps / 32768.0;
  config->calibration_value =
    trunc(0.00512 / (config->current_lsb * config->shunt_ohms));

  /*
    Configuration Register (0x00)
    Reset: (bit 15)
    Unused (bits 12-14)
    Averaging Mode: 1024 samples (bits 9-11: 111)
    Bus Voltage Conversion Time: 1.1ms (bits 6-8: 111)
    Shunt Voltage Conversion Time: 1.1ms (bits 3-5: 111)
    Operating Mode: Triggered shunt and bus (bits 0-2: 011)
    
    -> 0b 1 000 111 111 111 011
    ->      1000 1111 1111 1011
    config = 0x8F 0xFB
  */
  
  config->config_bytes[0] = 0x8F;
  config->config_bytes[1] = 0xFB;
  
  // I2C addresses for INA226 devices
  config->address = address; 	/* 0x44 pi_power, 0x45 battery_charging */
  snprintf(config->name, sizeof(config->name), "%s/%s", prefix, name);

#ifdef __linux__
  // trigger first conversion
  ina226_trigger_conversion(config);
  config->active = 1;
#endif
  return slot;			/* slot for this config */
}

static float ina226_read_voltage(ina226_config_t *config)
{
  // Read bus voltage and current
  uint16_t bus_voltage_raw = i2cReadWord16(config->fd, config->address, 0x02);
  float bus_voltage = bus_voltage_raw * 1.25 / 1000.0;  // LSB is 1.25mV
  return bus_voltage;
}

static float ina226_read_current(ina226_config_t *config)
{
  uint16_t current_raw = i2cReadWord16(config->fd, config->address, 0x04);
  if (current_raw > 32767)
    current_raw -= 65536;
  float current = current_raw * config->current_lsb;
  return current;
}

static int ina226_store_value(tclserver_t *tclserver,
		       float value, char *name)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%g", value);
  ds_datapoint_t *dp = dpoint_new(name,
				  tclserver_now(tclserver),
				  DSERV_STRING,
				  strlen(buf), (unsigned char *) buf);
  tclserver_set_point(tclserver, dp);
  return 0;
}

#ifdef __linux__
void *acquire_thread(void *arg)
{
  ina226_info_t *info = (ina226_info_t *) arg;

  uint64_t exp;
  ssize_t s;
  
  char point_name[256];
  float val;

  while (1) {
    s = read(info->timer_fd, &exp, sizeof(uint64_t));
    if (s == sizeof(uint64_t)) {
      
      for (int i = 0; i < MAX_INA226_CONFIGS; i++) {
	ina226_config_t *cfg = info->configs[i];
	if (cfg && cfg->active) {
	  if (ina226_conversion_complete(cfg)) {
	    val = ina226_read_voltage(cfg);
	    snprintf(point_name, "%s-v", config->name);
	    ina226_store_value(info->tclserver, point_name, val);

	    val = ina226_read_current(cfg);
	    snprintf(point_name, "%s-a", config->name);
	    ina226_store_value(info->tclserver, point_name, val);

	    // trigger next conversion
	    ina226_trigger_conversion(cfg);
	  }
	}
      }
    }
  }
}
#endif

static int ina226_add_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  ina226_info_t *info = (ina226_info_t *) data;
  int address;
  int result;
  
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "address prefix name");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &address) != TCL_OK)
    return TCL_ERROR;

  result =  ina226_initialize(info,
			      (uint8_t) address,
			      Tcl_GetString(objv[2]),
			      Tcl_GetString(objv[3]));
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  return TCL_OK;
}



/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_ina226_Init) (Tcl_Interp *interp)
#else
  int Dserv_ina226_Init(Tcl_Interp *interp)
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
  
  g_ina226Info.tclserver = tclserver_get();
  g_ina226Info.bus_number = 1;
  
  char i2cpath[255];
  snprintf(i2cpath, sizeof(i2cpath), "/dev/i2c-%d",
	   g_ina226Info.bus_number);

  // wake up every two seconds
  g_ina226Info.interval_sec = 2;

  // initialize slots to inactive
  for (int i = 0; i < MAX_INA226_CONFIGS; i++) {
    g_ina226Info.configs[i].active = 0;
  }
    
#ifdef __linux__
  // file descriptor for I2C read/write
  g_ina226Info.fd = open(i2cpath, O_RDWR);
  if (g_ina226Info.fd >= 0) {
    
    g_ina226Info.timer_fd = timerfd_create(CLOCK_REALTIME, 0);
    if (g_ina226Info.timer_fd == -1) return TCL_ERROR;
    
    struct itimerspec new_value;
    struct timespec now;
    
    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
      return TCL_ERROR;
    
    /* Create a CLOCK_REALTIME absolute timer with initial
       expiration and interval as specified in command line */
    
    new_value.it_value.tv_sec = now.tv_sec+g_ina226Info.interval_sec;
    new_value.it_value.tv_nsec = now.tv_nsec;
    new_value.it_interval.tv_sec = g_ina226Info.interval_sec;
    new_value.it_interval.tv_nsec = 0;
    
    timerfd_settime(g_ina226Info.timer_fd, TFD_TIMER_ABSTIME, &new_value, NULL);

    if (pthread_create(&g_ina226Info.timer_thread_id, NULL, acquire_thread,
		       (void *) &g_ina226Info)) {
      return TCL_ERROR;
    }
    pthread_detach(g_ina226Info.timer_thread_id);
  }
#else
  g_ina226Info.fd = -1;
  g_ina226Info.timer_fd = -1;
#endif
  
  Tcl_CreateObjCommand(interp, "ina226Add",
		       (Tcl_ObjCmdProc *) ina226_add_command,
		       (ClientData) &g_ina226Info,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}
