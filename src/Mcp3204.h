#ifndef MCP3204_H
#define MCP3204_H

#include "Datapoint.h"
#include "bus/SPIDevice.h"
using namespace exploringRPi;

class Mcp3204 {

private:
  SPIDevice *busDevice;
    
public:
  Mcp3204();
  ~Mcp3204();
  int read(int nchan, uint16_t *buf);
};

#endif
