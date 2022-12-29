#ifndef WIFINTP_H
#define WIFINTP_H

#include <time.h>
#include <WiFiUDP.h>

class WiFiNTP
{
  public:
    int8_t timeZoneOffset = 0; // Legacy (no longer used)
    const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Amsterdam TZ
    const char* NTPServer = nullptr;

    // Constructors
    WiFiNTP();
    WiFiNTP(int serverSyncInterval);
    WiFiNTP(const char* ntpServer, int serverSyncInterval);

    bool beginGetServerTime();
    time_t endGetServerTime();
    time_t getServerTime();
    time_t getCurrentTime();

  private:
    int _serverSyncInterval;
    bool _isInitialized = false;
    bool _serverTimeReceived = false;

    void initialize();
    void onServerTimeReceived();

    friend void _settimeofday_callback();
};

#endif