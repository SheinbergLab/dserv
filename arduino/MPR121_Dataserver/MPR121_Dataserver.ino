#include <Base64.h>
#include <Wire.h>

#include "Adafruit_MPR121.h"

#ifndef _BV
#define _BV(bit) (1 << (bit)) 
#endif

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

// C++ includes
#include <cstdint>

#include <Print.h>

#include <QNEthernet.h>

#define DPOINT_BINARY_MSG_CHAR '>'
#define DPOINT_BINARY_FIXED_LENGTH (128)

// toggle this pin
const int outputPin = 32;

namespace qn = qindesign::network;

#define TIMER_INTERVAL_US 20000
volatile bool expired = false;

// --------------------------------------------------------------------------
//  MPR121 touch sensor support
// --------------------------------------------------------------------------

// You can have up to 4 on one i2c bus but one is enough for testing!
Adafruit_MPR121 cap0 = Adafruit_MPR121();
Adafruit_MPR121 cap1 = Adafruit_MPR121();

// --------------------------------------------------------------------------
//  Configuration
// --------------------------------------------------------------------------

// The DHCP timeout, in milliseconds. Set to zero to not wait and
// instead rely on the listener to inform us of an address assignment.
constexpr uint32_t kDHCPTimeout = 10000;  // 10 seconds

// The link timeout, in milliseconds. Set to zero to not wait and
// instead rely on the listener to inform us of a link.
constexpr uint32_t kLinkTimeout = 5000;  // 5 seconds

const int DSERV_PORT = 4620;

//IPAddress staticIP{192, 168, 88, 177};
IPAddress staticIP = INADDR_NONE;
IPAddress subnetMask{255, 255, 255, 0};
IPAddress gateway{192, 168, 88, 1};

//char dataserverAddress[] = "192.168.88.252";
char dataserverAddress[] = "192.168.88.40";
qn::EthernetClient client;
IntervalTimer ITimer;

static bool do_reconnect = false;  // set to true when new address is assigned
void TimerExpired()
{
  expired = true;
}

typedef enum {
  DSERV_BYTE = 0,
  DSERV_STRING,
  DSERV_FLOAT,
  DSERV_DOUBLE,
  DSERV_SHORT,
  DSERV_INT,
  DSERV_DG,
  DSERV_SCRIPT,
  DSERV_TRIGGER_SCRIPT,		/* will always be delivered to trigger thread */
  DSERV_EVT,
  DSERV_NONE,
  DSERV_UNKNOWN,
} ds_datatype_t;

int write_to_dataserver(qn::EthernetClient client, const char *varname, int dtype, int len, void *data) 
{
  uint8_t cmd = DPOINT_BINARY_MSG_CHAR;
  static char buf[DPOINT_BINARY_FIXED_LENGTH];
 
  uint16_t varlen;
  uint64_t timestamp = micros();
  uint32_t datatype = dtype,  datalen = len;

  uint16_t bufidx = 0;
  uint16_t total_bytes = 0;

  varlen = strlen(varname);

  // Start by seeing how much space we need
  total_bytes += sizeof(uint16_t); // varlen
  total_bytes += varlen;           // strlen(varname)
  total_bytes += sizeof(uint64_t); // timestamp
  total_bytes += sizeof(uint32_t); // datatype
  total_bytes += sizeof(uint32_t); // datalen
  total_bytes += len;              // data

  // data don't fit
  if (total_bytes > sizeof(buf)-1) {
    return 0;
  }

  memcpy(&buf[bufidx], &cmd, sizeof(uint8_t));
  bufidx += sizeof(uint8_t);

  memcpy(&buf[bufidx], &varlen, sizeof(uint16_t));
  bufidx += sizeof(uint16_t);

  memcpy(&buf[bufidx], varname, varlen);
  bufidx += varlen;

  memcpy(&buf[bufidx], &timestamp, sizeof(uint64_t));
  bufidx += sizeof(uint64_t);

  memcpy(&buf[bufidx], &datatype, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], &datalen, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], data, datalen);
  bufidx += datalen;

  if (client.write(buf, sizeof(buf)) < 0)
  {
    printf("writing socket");
    return 0;
  }
  client.flush();
  return 1;
}
int send_to_dataserver(qn::EthernetClient client, char *var, int dtype, int n, void *data)
{
  static char buf[1024];
  static char sendbuf[2048];
  int datalen;
  int eltsizes[] { 1, 0, 4, 0, 2, 4 };
  if (dtype != 0 && dtype != 2 && dtype != 4 && dtype != 5) return -1;
  datalen = n*eltsizes[dtype];
  if ((unsigned int) Base64.encodedLength(datalen) > sizeof(buf)) return 0;
  Base64.encode(buf, (char *) data, datalen);
#ifdef LOCAL_TIMESTAMP
  sprintf(sendbuf, "%%setdata %s %d %u %d {%s}\r\n", var, dtype,
          (unsigned int) micros(), datalen, buf);
#else
  sprintf(sendbuf, "%%setdata %s %d %u %d {%s}\r\n", var, dtype,
          0, datalen, buf);
#endif
#if 1
   if (client.connected()) {
      client.write(sendbuf, strlen(sendbuf));
      client.flush();
      while (client.available()) { 
        client.read(); 
      }
    }
    else {
      client.stop();
    }
  
#else
Serial.print(sendbuf);
#endif
return 1;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) {
    // Wait for Serial to initialize
  }

  delay(250);

  // Default address is 0x5A, if tied to 3.3V its 0x5B
  // If tied to SDA its 0x5C and if SCL then 0x5D
  if (!cap0.begin(0x5A)) {
    Serial.println("MPR121 not found, check wiring?");
    while (1);
  }
  Serial.println("MPR121[0] found!");

  if (!cap1.begin(0x5B)) {
    Serial.println("2nd MPR121 not found, check wiring?");
    while (1);
  }
  Serial.println("MPR121[1] found!");

  //qn::stdPrint = &Serial;  // Make printf work (a QNEthernet feature)
  printf("Starting...\n");

  // Unlike the Arduino API (which you can still use), QNEthernet uses
  // the Teensy's internal MAC address by default, so we can retrieve
  // it here
  uint8_t mac[6];
  qn::Ethernet.macAddress(mac);  // This is informative; it retrieves, not sets
  printf("MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Listen for link changes
  qn::Ethernet.onLinkState([](bool state) {
    printf("[Ethernet] Link %s\n", state ? "ON" : "OFF");
  });

  // Listen for address changes
  qn::Ethernet.onAddressChanged([]() {
    IPAddress ip = qn::Ethernet.localIP();
    bool hasIP = (ip != INADDR_NONE);
    if (hasIP) {
      printf("[Ethernet] Address changed:\n");

      IPAddress ip = qn::Ethernet.localIP();
      printf("    Local IP = %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
      ip = qn::Ethernet.subnetMask();
      printf("    Subnet   = %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
      ip = qn::Ethernet.gatewayIP();
      printf("    Gateway  = %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
      ip = qn::Ethernet.dnsServerIP();
      if (ip != INADDR_NONE) {  // May happen with static IP
        printf("    DNS      = %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
      }
      
      do_reconnect = true;

    } else {
      printf("[Ethernet] Address changed: No IP address\n");
    }


  });

  if (staticIP == INADDR_NONE) {
    printf("Starting Ethernet with DHCP...\n");
    if (!qn::Ethernet.begin()) {
      printf("Failed to start Ethernet\n");
      return;
    }

    // We can choose not to wait and rely on the listener to tell us
    // when an address has been assigned
    if (kDHCPTimeout > 0) {
      if (!qn::Ethernet.waitForLocalIP(kDHCPTimeout)) {
        printf("Failed to get IP address from DHCP\n");
        // We may still get an address later, after the timeout,
        // so continue instead of returning
      }
    }
  } else {
    printf("Starting Ethernet with static IP...\n");
    qn::Ethernet.begin(staticIP, subnetMask, gateway);

    // When setting a static IP, the address is changed immediately,
    // but the link may not be up; optionally wait for the link here
    if (kLinkTimeout > 0) {
      if (!qn::Ethernet.waitForLink(kLinkTimeout)) {
        printf("Failed to get link\n");
        // We may still see a link later, after the timeout, so
        // continue instead of returning
      }
    }
    else {
      do_reconnect = true;
    }
  }

  pinMode(outputPin, OUTPUT);

  // Interval in microsecs
  ITimer.begin(TimerExpired, TIMER_INTERVAL_US);
}

void loop() {
  const int NSENSORS = 6;
  // Keeps track of the last pins touched
  // so we know when buttons are 'released'
  static uint16_t lasttouched0 = 0;
  static uint16_t currtouched0 = 0;
  static uint16_t lasttouched1 = 0;
  static uint16_t currtouched1 = 0;
  static uint16_t filtered_data[12];

  static const char *sensor0_touched_point = "grasp/sensor0/touched";
  static const char *sensor0_vals_point = "grasp/sensor0/vals";
  static const char *sensor1_touched_point = "grasp/sensor1/touched";
  static const char *sensor1_vals_point = "grasp/sensor1/vals";

  if (do_reconnect) {
   if (client.connect(dataserverAddress, DSERV_PORT)) {
      printf("connected\n");
    } else {
      printf("connection failed\n");
    }
    client.setNoDelay(true);
    do_reconnect = false;
  }

  // Get the currently touched pads
  currtouched0 = cap0.touched();
  if (currtouched0 != lasttouched0) {
    if (client.connected()) {
      write_to_dataserver(client, sensor0_touched_point, DSERV_SHORT, sizeof(uint16_t), &currtouched0);
    }
    lasttouched0 = currtouched0;
  }
    
  currtouched1 = cap1.touched();
  if (currtouched1 != lasttouched1) {
    if (client.connected()) {
      write_to_dataserver(client, sensor1_touched_point, DSERV_SHORT, sizeof(uint16_t), &currtouched1);
    }
    lasttouched1 = currtouched1;
  }

  if (expired) {
    for (int i = 0; i < NSENSORS; i++) {
      filtered_data[i] = cap0.filteredData(i);
    }
    write_to_dataserver(client, sensor0_vals_point, DSERV_SHORT, NSENSORS*sizeof(uint16_t), &filtered_data[0]);

    for (int i = 0; i < NSENSORS; i++) {
      filtered_data[i] = cap1.filteredData(i);
    }
    write_to_dataserver(client, sensor1_vals_point, DSERV_SHORT, NSENSORS*sizeof(uint16_t), &filtered_data[0]);

    noInterrupts();
    expired = false;
    interrupts();
  }


}

