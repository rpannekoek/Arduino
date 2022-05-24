#include <WiFiNTP.h>
#include <ESPWiFi.h>
#include <Arduino.h>
#include <Tracer.h>
#include <coredecls.h>  // settimeofday_cb()
#include <sntp.h>

WiFiNTP* _instancePtr = nullptr;

void _settimeofday_callback()
{
    if (_instancePtr != nullptr)
        _instancePtr->onServerTimeReceived();
}

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
    settimeofday_cb(_settimeofday_callback);
    // TODO: use _serverSyncInterval
    // sntp_set_update_delay(_serverSyncInterval * 1000);
    configTime(timeZone, NTPServer);
    _isInitialized = true;
}


void WiFiNTP::onServerTimeReceived()
{
    Tracer tracer(F("WiFiNTP::onServerTimeReceived"));

    if (!_serverTimeReceived)
    {
        // First time we receive NTP server time
        IPAddress ntpServerIP = sntp_getserver(0)->addr;
        const char* ntpServerName = sntp_getservername(0);

        NTPServer = (ntpServerName == nullptr)
            ? ntpServerIP.toString().c_str()
            : ntpServerName; 

        TRACE("NTP Server: '%s'\n", NTPServer);
    }

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
