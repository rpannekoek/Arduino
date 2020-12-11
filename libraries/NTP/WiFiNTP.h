#ifndef WIFINTP_H
#define WIFINTP_H

#include <time.h>
#include <WiFiUDP.h>

class WiFiNTP
{
  public:
    int8_t timeZoneOffset = 0;

    // Constructors
    WiFiNTP(int serverSyncInterval);
    WiFiNTP(const char* ntpServer, int serverSyncInterval);

    const char* NTPServer = nullptr;

    bool beginGetServerTime();
    time_t endGetServerTime();
    time_t getServerTime();
    time_t getCurrentTime();

  private:
    static const unsigned int LOCAL_PORT = 2390;
    static const int NTP_PACKET_SIZE = 48;

    WiFiUDP _udp;
    const char* _ntpServer = nullptr;
    IPAddress _timeServerIP;
    uint8_t _packetBuffer[NTP_PACKET_SIZE];
    long  _serverSyncInterval;
    long _lastServerSync;
    long _lastServerTry;
    time_t _lastServerTime;

    void sendPacket();
    unsigned long readPacket();
};

#endif