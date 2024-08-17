#include "TeensyTimerTool.h"
#include <Base64.h>

using namespace TeensyTimerTool;

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

// for command parsing
String inputString = "";         // a String to hold incoming data

// some arbitrary pins to test round trip time to computer
int output_pin = 20;
int acknowledge_pin = 6;

int send_to_dserv_txt(char *var, int dtype, int n, void *data)
{
  static char buf[1024];
  static char sendbuf[2048];
  int datalen;
  int eltsizes[] { 1, 0, 4, 0, 2, 4 };
  if (dtype != 0 && dtype != 2 && dtype != 4 && dtype != 5) return -1;
  if (n) {
    datalen = n*eltsizes[dtype];
    if ((unsigned int) Base64.encodedLength(datalen) > sizeof(buf)) return 0;
    Base64.encode(buf, (char *) data, datalen);
  }
  else {
    datalen = 0;
    buf[0] = '\0';
  }
  sprintf(sendbuf, "%%setdata %s %d %u %d {%s}", var, dtype, 
          0 /*(unsigned int) micros()*/, datalen, buf);
  Serial.println(sendbuf);
  return 1;
}

void timer_alert(int id)
{
  static char varname[32];
#if 0
  static int status = 0;
  status = !status;
  digitalWriteFast(id, status);
  snprintf_P(varname, sizeof(varname), "gpio/input/%d", id);
  send_to_dserv_txt(varname, DSERV_INT, 1, &status);
#endif
  uint16_t vals[2];
  vals[0] = analogRead(A0);
  vals[1] = analogRead(A2);
  snprintf_P(varname, sizeof(varname), "ain/vals");
  send_to_dserv_txt(varname, DSERV_SHORT, 2, vals);
}

void callback()
{
    //digitalWriteFast(LED_BUILTIN, !digitalReadFast(LED_BUILTIN));
    timer_alert(output_pin);
}


/**
 * @brief Read available serial port (over USB) input up to newline
 * 
 * @return None, but set stringComplete to true if newline was reached
 */

bool checkInput() {

  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read();
    // add it to the inputString:
    inputString += inChar;
    // if the incoming character is a newline, return true
    if (inChar == '\n') {
      return true;
    }
  }
  return false;
}

int processInput(void)
{
  inputString.trim();

  if (inputString.startsWith("LED ")) {
    if (inputString.substring(4).startsWith("1")) {
        digitalWriteFast(LED_BUILTIN, 1);
    }
    else if (inputString.substring(4).startsWith("0")) {
        digitalWriteFast(LED_BUILTIN, 0);
    }
  }
  else if (inputString.startsWith("DHI ")) {
    int pin = inputString.substring(4).toInt();
    if (pin) digitalWriteFast(pin, 1);
  }
  else if (inputString.startsWith("DLO")) {
    int pin = inputString.substring(4).toInt();
    if (pin) digitalWriteFast(pin, 0);
  }

  // clear the string:
  inputString = "";

  return 0;
}

PeriodicTimer t1; // generate a timer from the pool (Pool: 2xGPT, 16xTMR(QUAD), 20xTCK)

void setup()
{
  Serial.begin(115200);
  pinMode(LED_BUILTIN,OUTPUT);

  analogReadResolution(12);

  pinMode(output_pin,OUTPUT);
  pinMode(acknowledge_pin,OUTPUT);

  digitalWriteFast(LED_BUILTIN, 1);
  t1.begin(callback, 1'000); // 10ms
}

void loop() {
  if (checkInput()) processInput();
}
