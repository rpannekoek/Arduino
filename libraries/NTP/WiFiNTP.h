#ifndef WIFINTP_H
#define WIFINTP_H

#include <time.h>
#include <WiFiUDP.h>

class WiFiNTP
{
  public:
    WiFiNTP(const char* timeServerPool, int serverSyncInterval);
    time_t getServerTime();
    time_t getCurrentTime();

  private:
    static const unsigned int LOCAL_PORT = 2390;
    static const int NTP_PACKET_SIZE = 48;

    WiFiUDP _udp;
    const char* _timeServerPool;
    IPAddress _timeServerIP;
    uint8_t _packetBuffer[NTP_PACKET_SIZE];
    unsigned long  _serverSyncInterval;
    unsigned long _lastServerSync;
    unsigned long _lastServerTry;
    time_t _lastServerTime;

    void sendPacket();
    unsigned long readPacket();
};

#endif