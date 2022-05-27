#include <WiFiNTP.h>
#include <ESPWiFi.h>
#include <Arduino.h>
#include <Tracer.h>
#include <sntp.h>

WiFiNTP* _instancePtr = nullptr;

void _settimeofday_callback()
{
    if (_instancePtr != nullptr)
        _instancePtr->onServerTimeReceived();
}

#ifdef ESP8266
#include <coredecls.h>  // settimeofday_cb()
#else 
void _sntp_sync_time_cb(timeval* tv)
{
    _settimeofday_callback();
}
#endif


// Constructor
WiFiNTP::WiFiNTP()
{
    _serverSyncInterval = 0;
}

// Constructor
WiFiNTP::WiFiNTP(int serverSyncInterval)
{
    _serverSyncInterval = serverSyncInterval;
}

// Constructor
WiFiNTP::WiFiNTP(const char* ntpServer, int serverSyncInterval)
{
    NTPServer = ntpServer;
    _serverSyncInterval = serverSyncInterval;
}


void WiFiNTP::initialize()
{
    Tracer tracer(F("WiFiNTP::initialize"));
    _instancePtr = this;
    // TODO: use _serverSyncInterval
    // sntp_set_update_delay(_serverSyncInterval * 1000);
#ifdef ESP8266
    settimeofday_cb(_settimeofday_callback);
    configTime(timeZone, NTPServer);
#else
    sntp_set_time_sync_notification_cb(_sntp_sync_time_cb);
    configTzTime(timeZone, NTPServer);
#endif
    _isInitialized = true;
}


void WiFiNTP::onServerTimeReceived()
{
    Tracer tracer(F("WiFiNTP::onServerTimeReceived"));

    _serverTimeReceived = true;
}


bool WiFiNTP::beginGetServerTime()
{
    Tracer tracer(F("WiFiNTP::beginGetServerTime"));

    if (!_isInitialized)
    {
        initialize();
    }

    return true;
}


time_t WiFiNTP::endGetServerTime()
{
    return _serverTimeReceived ? time(nullptr) : (time_t)0;
}


time_t WiFiNTP::getServerTime()
{
    Tracer tracer(F("WiFiNTP::getServerTime"));

    if (!beginGetServerTime())
        return 0;

    TRACE(F("Awaiting NTP server response..."));
    for (int i = 0; i < 20; i++)
    {
        time_t result = endGetServerTime();
        if (result != 0)
            return result;
        TRACE(".");
        delay(100);
    }

    TRACE(F("\nTimeout waiting for NTP Server response.\n"));
    return 0;
}


time_t WiFiNTP::getCurrentTime()
{
    return time(nullptr);
}
