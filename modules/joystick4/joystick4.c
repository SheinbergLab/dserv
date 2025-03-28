/*
 * NAME
 *   joystick4.c
 *
 * DESCRIPTION
 *   This module reads joystick/button info from the Mikroe Joystick4 (6279)
 *  values to dserv.
 *
 * DPOINTS
 *   joystick4/val
 *
 * AUTHOR
 *   DLS, 03/25
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
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#endif

/* register mapping */
#define JOYSTICK4_REG_INPUT                 0x00
#define JOYSTICK4_REG_OUTPUT                0x01
#define JOYSTICK4_REG_POLARITY              0x02
#define JOYSTICK4_REG_CONFIG                0x03

/* pca9538 pin mask */
#define JOYSTICK4_PIN_A                     0x20
#define JOYSTICK4_PIN_B                     0x08
#define JOYSTICK4_PIN_C                     0x80
#define JOYSTICK4_PIN_D                     0x10
#define JOYSTICK4_PIN_CE                    0x40

#define JOYSTICK4_DEFAULT_PIN_CONFIG        0xFF

/* Joystick 4 pin logic state setting */
#define JOYSTICK4_PIN_STATE_LOW             0
#define JOYSTICK4_PIN_STATE_HIGH            1

/* Joystick 4 position values */
#define JOYSTICK4_POSITION_IDLE             0
#define JOYSTICK4_POSITION_CENTER           1
#define JOYSTICK4_POSITION_CENTER_UP        2
#define JOYSTICK4_POSITION_CENTER_RIGHT     3
#define JOYSTICK4_POSITION_CENTER_DOWN      4
#define JOYSTICK4_POSITION_CENTER_LEFT      5
#define JOYSTICK4_POSITION_UP               6
#define JOYSTICK4_POSITION_UPPER_RIGHT      7
#define JOYSTICK4_POSITION_RIGHT            8
#define JOYSTICK4_POSITION_LOWER_RIGHT      9
#define JOYSTICK4_POSITION_DOWN             10
#define JOYSTICK4_POSITION_LOWER_LEFT       11
#define JOYSTICK4_POSITION_LEFT             12
#define JOYSTICK4_POSITION_UPPER_LEFT       13

/* Joystick 4 device address setting */
#define JOYSTICK4_DEVICE_ADDRESS_0          0x20
#define JOYSTICK4_DEVICE_ADDRESS_1          0x21


#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

int i2cReadRegister(int i2cfd, uint8_t slaveaddr, uint8_t reg,
		    uint8_t *buf, int len)
{
#ifdef __linux__
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data msgset[1];

  msgs[0].addr = slaveaddr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[1].addr = slaveaddr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = len;
  msgs[1].buf = buf;

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
  struct i2c_msg msgs[1];
  struct i2c_rdwr_ioctl_data msgset[1];

  /* large i2c transfers aren't supported by this device */
  if (len <= 32) {
    uint8_t data[33];
    data[0] = reg;
    memcpy(&data[1], buf, len);
    
    msgs[0].addr = slaveaddr;
    msgs[0].flags = 0;
    msgs[0].len = 1+len;
    msgs[0].buf = data;
    
    msgset[0].msgs = msgs;
    msgset[0].nmsgs = 1;
    
    if (ioctl(i2cfd, I2C_RDWR, &msgset) < 0) {
      return -1;
    }
  }
#endif
  return 0;
}


int i2cWriteWord16(int i2cfd, uint8_t slaveaddr, uint8_t reg,
		 uint16_t data)
{
  uint8_t buf[2];
  buf[0] = (data & 0xFF00) >> 8;
  buf[1] = (data & 0x00FF);
  return i2cWriteRegister(i2cfd, slaveaddr, reg, buf, 2);
}

uint16_t i2cReadWord16(int i2cfd, uint8_t slaveaddr, uint8_t reg)
{
  uint8_t buf[2];
  i2cReadRegister(i2cfd, slaveaddr, reg, buf, 2);
  return ((uint16_t) (buf[0] << 8 | buf[1]));
}

int i2cWriteByte(int i2cfd,
		 uint8_t slaveaddr,
		 uint8_t reg,
		 uint8_t data)
{
  return i2cWriteRegister(i2cfd, slaveaddr, reg, &data, 1);
}

int i2cReadByte(int i2cfd, uint8_t slaveaddr, uint8_t reg, uint8_t *out)
{
  uint8_t byte;
  int result;
  result = i2cReadRegister(i2cfd, slaveaddr, reg, &byte, 1);
  if (out) *out = byte;
  return result;
}

typedef struct joystick4_config_s {
  atomic_int active;

  // file descriptor for I2C read/write
  int fd;

  // server for storing points to
  tclserver_t *tclserver;

  // interrupt (int)
  int interrupt_pin;

  // reset (out)
  int reset_pin;
  
  // val
  int value;
  
  // I2C addresses for INA226 devices
  uint8_t address;

  char name[128];
} joystick4_config_t;

typedef struct joystick4_info_s
{
  tclserver_t *tclserver;
  
  // file descriptor for I2C read/write
  int fd;
  
  // I2C bus number
  int bus_number;
  
  // slots for up to MAX_INS226 configurations
  joystick4_config_t config;

} joystick4_info_t;

/* global to this module */
static joystick4_info_t g_joystick4Info;

int joystick4_write_reg(joystick4_config_t *config,
			uint8_t reg, uint8_t val)
{
  return i2cWriteByte(config->fd, config->address, reg, val);
}


int joystick4_read_reg(joystick4_config_t *config,
		       uint8_t reg, uint8_t *data_out)
{
  return i2cReadByte(config->fd, config->address, reg, data_out);
}

int joystick4_get_pins(joystick4_config_t *config, uint8_t *pin_mask)
{
  return joystick4_read_reg(config, JOYSTICK4_REG_INPUT, pin_mask);
}

uint8_t joystick4_get_position ( uint8_t pin_mask )
{
  uint8_t position = JOYSTICK4_POSITION_IDLE;
  if ( JOYSTICK4_PIN_CE != ( pin_mask & JOYSTICK4_PIN_CE ) )
    {
      if ( JOYSTICK4_PIN_A != ( pin_mask & JOYSTICK4_PIN_A ) )
        {
	  position = JOYSTICK4_POSITION_CENTER_UP;
        }
      else if ( JOYSTICK4_PIN_B != ( pin_mask & JOYSTICK4_PIN_B ) )
        {
	  position = JOYSTICK4_POSITION_CENTER_RIGHT;
        }
      else if ( JOYSTICK4_PIN_C != ( pin_mask & JOYSTICK4_PIN_C ) )
        {
	  position = JOYSTICK4_POSITION_CENTER_LEFT;
        }
      else if ( JOYSTICK4_PIN_D != ( pin_mask & JOYSTICK4_PIN_D ) )
        {
	  position = JOYSTICK4_POSITION_CENTER_DOWN;
        }
      else
        {
	  position = JOYSTICK4_POSITION_CENTER;
        }
    }
  else if ( JOYSTICK4_PIN_A != ( pin_mask & JOYSTICK4_PIN_A ) )
    {
      if ( JOYSTICK4_PIN_B != ( pin_mask & JOYSTICK4_PIN_B ) )
        {
	  position = JOYSTICK4_POSITION_UPPER_RIGHT;
        }
      else if ( JOYSTICK4_PIN_C != ( pin_mask & JOYSTICK4_PIN_C ) )
        {
	  position = JOYSTICK4_POSITION_UPPER_LEFT;
        }
      else
        {
	  position = JOYSTICK4_POSITION_UP;
        }
    }
  else if ( JOYSTICK4_PIN_B != ( pin_mask & JOYSTICK4_PIN_B ) )
    {
      if ( JOYSTICK4_PIN_D != ( pin_mask & JOYSTICK4_PIN_D ) )
        {
	  position = JOYSTICK4_POSITION_LOWER_RIGHT;
        }
      else
        {
	  position = JOYSTICK4_POSITION_RIGHT;
        }
    }
  else if ( JOYSTICK4_PIN_C != ( pin_mask & JOYSTICK4_PIN_C ) )
    {
      if ( JOYSTICK4_PIN_D != ( pin_mask & JOYSTICK4_PIN_D ) )
        {
	  position = JOYSTICK4_POSITION_LOWER_LEFT;
        }
      else
        {
	  position = JOYSTICK4_POSITION_LEFT;
        }
    }
  else if ( JOYSTICK4_PIN_D != ( pin_mask & JOYSTICK4_PIN_D ) )
    {
      position = JOYSTICK4_POSITION_DOWN;
    }
  return position;
}


static int joystick4_initialize(joystick4_info_t *joystick4info,
				uint8_t address)
{
  joystick4_config_t *config = NULL;
  if (joystick4info->fd < 0) return -1;

  config = &joystick4info->config;

  /* copy fd so we can access from config */
  config->fd = joystick4info->fd;
  
  /* copy tclserver so we can use to store datapoints */
  config->tclserver = joystick4info->tclserver;
  
  // I2C addresses for joystick4 devices
  config->address = address; 	/* 0x20, 0x21 */
  
#ifdef __linux__
  // Configure all pins as INPUT
  return joystick4_write_reg (config,
			      JOYSTICK4_REG_CONFIG,
			      JOYSTICK4_DEFAULT_PIN_CONFIG);
  config->active = 1;
#endif
  return 0;
}


static int joystick4_read_command(ClientData data,
				  Tcl_Interp *interp,
				  int objc, Tcl_Obj *objv[])
{
  joystick4_info_t *info = (joystick4_info_t *) data;
  
  if (info->fd < 0) {
    return TCL_OK;
  }

  uint8_t pin_mask, position;
  int result = joystick4_get_pins(&info->config, &pin_mask);
  if (!result) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": error reading joystick4", NULL);
    return TCL_ERROR;
  }
  position = joystick4_get_position(pin_mask);
  
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(position));
  return TCL_OK;
}


/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_joystick_Init) (Tcl_Interp *interp)
#else
  int Dserv_joystick_Init(Tcl_Interp *interp)
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
  
  g_joystick4Info.tclserver = tclserver_get();
  g_joystick4Info.bus_number = 1;
  
  char i2cpath[255];
  snprintf(i2cpath, sizeof(i2cpath), "/dev/i2c-%d",
	   g_joystick4Info.bus_number);

  g_joystick4Info.config.active = 0;
  
#ifdef __linux__
  // file descriptor for I2C read/write
  g_joystick4Info.fd = open(i2cpath, O_RDWR);
  if (g_joystick4Info.fd >= 0) {
    joystick4_initialize(&g_joystick4Info, JOYSTICK4_DEVICE_ADDRESS_0);
  }
#else
  g_joystick4Info.fd = -1;
#endif
  
  Tcl_CreateObjCommand(interp, "joystick4Read",
		       (Tcl_ObjCmdProc *) joystick4_read_command,
		       (ClientData) &g_joystick4Info,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}
