/* Using an SPI ADC (e.g., the MCP3208) */

#include <iostream>
#include <chrono>
#include <functional>
#include <cstring>
#include <future>

#include <sys/time.h>		/* for setitimer */
#include <unistd.h>		/* for pause */

#include <signal.h>		/* for signal */
#include <stdlib.h>
#include <stdio.h>

#include "Mcp3204.h"

Mcp3204::Mcp3204(void)
{
  using namespace std::placeholders;
  
  busDevice = new SPIDevice(0,0);
  busDevice->setSpeed(5000000);
  busDevice->setMode(SPIDevice::MODE0);
}

Mcp3204::~Mcp3204() {
  delete busDevice;
}

int Mcp3204::read(int nchan, uint16_t *buf)
{
  unsigned char send[3], receive[3];
  send[0] = 0b00000110;     // Reading single-ended input from channel 0
  int chan = 1;
  for(int i=0; i<nchan; i++) {
    send[1] = (i << 6);
    busDevice->transfer(send, receive, 3);
    buf[i] = (uint16_t) ((receive[1]&0b00001111)<<8)|receive[2];
  }
  return 0;
}


  

