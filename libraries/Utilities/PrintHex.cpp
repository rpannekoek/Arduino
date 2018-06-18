#include <Arduino.h>

void printHex(uint8_t* buf, int length)
{
  for (int i = 0; i < length; i++)
  {
    Serial.printf("%02X ", buf[i]);
  }
  Serial.println();
}
