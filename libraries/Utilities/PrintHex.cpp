#include <Arduino.h>
#include <Tracer.h>

void printHex(uint8_t* buf, int length)
{
  for (int i = 0; i < length; i++)
  {
    TRACE("%02X ", buf[i]);
  }
  TRACE("\n");
}
