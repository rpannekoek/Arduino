#include <WiFiNTP.h>
#include <ESP8266WiFi.h>
#include <Arduino.h>
#include <Tracer.h>

// Constructor
WiFiNTP::WiFiNTP(const char* timeServerPool, int serverSyncInterval)
{
  _timeServerPool = timeServerPool;
  _serverSyncInterval = serverSyncInterval;
  _lastServerSync = 0;
  _lastServerTry = 0;
}
    

time_t WiFiNTP::getServerTime()
{
  Tracer tracer("WiFiNTP::getServerTime");

  TRACE("Resolving NTP server name '%s' ...\n", _timeServerPool);
  if (!WiFi.hostByName(_timeServerPool, _timeServerIP))
  {
    TRACE("Unable to resolve DNS name.\n");
    return 0;
  } 

  _udp.begin(LOCAL_PORT);
  
  sendPacket();

  TRACE("Awaiting NTP server response...");
  int packetSize = 0;
  for (int i = 0; i < 50; i++)
  {
    packetSize = _udp.parsePacket();
    if (packetSize != 0)
      break;
    TRACE(".");
    delay(100);
  }

  if (packetSize == 0)
  {
    TRACE("\nTimeout waiting for NTP Server response.\n");
    return 0;
  }
  TRACE("\nPacket received. Size: %d bytes.\n", packetSize);

  unsigned long secondsSince1900 = readPacket();

  _udp.stopAll();
  
  const unsigned long SEVENTY_YEARS = 2208988800UL;
  return static_cast<time_t>(secondsSince1900 - SEVENTY_YEARS);
}


time_t WiFiNTP::getCurrentTime()
{
    long currentTime = millis() / 1000;

    if (currentTime < _lastServerSync)
    {
      // Internal clock rollover (occurs approx. each 50 days)
      TRACE("Internal clock rollover.\n");
      const long MAX_TIME = 4294967L;
      _lastServerSync -= MAX_TIME;
      _lastServerTry -= MAX_TIME;
    }

    if ((currentTime >= (_lastServerSync + _serverSyncInterval)) || (_lastServerSync == 0))
    {
        // Server sync needed, but don't try server more than each minute.
        if ((currentTime >= (_lastServerTry + 60)) || (_lastServerTry == 0))
        {
          time_t serverTime = getServerTime();
          if (serverTime != 0)
          {
              TRACE("Time synchronized with server: %u\n", serverTime);
              _lastServerTime = serverTime;
              _lastServerSync = currentTime;
          }
          _lastServerTry = currentTime;
        }
    }

    return _lastServerTime + (currentTime - _lastServerSync);
}

void WiFiNTP::sendPacket()
{
  memset(_packetBuffer, 0, NTP_PACKET_SIZE);
  _packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  _packetBuffer[1] = 0;     // Stratum, or type of clock
  _packetBuffer[2] = 6;     // Polling Interval
  _packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  _packetBuffer[12]  = 49;
  _packetBuffer[13]  = 0x4E;
  _packetBuffer[14]  = 49;
  _packetBuffer[15]  = 52;

  _udp.beginPacket(_timeServerIP, 123); //NTP requests are to port 123
  _udp.write(_packetBuffer, NTP_PACKET_SIZE);
  _udp.endPacket();
}


unsigned long WiFiNTP::readPacket()
{
    _udp.read(_packetBuffer, NTP_PACKET_SIZE);

    // The timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, esxtract the two words:
    unsigned long highWord = word(_packetBuffer[40], _packetBuffer[41]);
    unsigned long lowWord = word(_packetBuffer[42], _packetBuffer[43]);
    // Combine the four bytes (two words) into a long integer. This is NTP time (seconds since Jan 1 1900):
    return  highWord << 16 | lowWord;
}

