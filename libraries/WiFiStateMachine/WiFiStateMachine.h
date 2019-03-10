#ifndef WIFI_STATE_MACHINE_H
#define WIFI_STATE_MACHINE_H

#include <stdint.h>
#include <ESPWebServer.h>
#include <WiFiNTP.h>
#include <Log.h>


enum struct WiFiState
{
    Booting = 0,
    Initializing = 1,
    Connecting = 2,
    ConnectFailed = 3,
    Connected = 4,
    TimeServerInitializing = 5,
    TimeServerSyncing = 6,
    TimeServerSyncFailed = 7,
    TimeServerSynced = 8,
    Initialized = 9
};


class WiFiStateMachine
{
    public:
        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, WebServer& webServer, Log<const char>& eventLog);

        void on(WiFiState state, void (*handler)(void));
 
        void begin(String ssid, String password, String hostName);
        void run();

        void logEvent(String msg);
        time_t getCurrentTime();

        time_t getInitTime()
        {
            return _initTime;
        }

        uint32_t getUptime()
        {
            return getCurrentTime() - _initTime;
        }

        WiFiState getState()
        {
            return _state;
        }

    protected:
        WiFiState _state = WiFiState::Booting;
        uint32_t _stateChangeTime = 0;
        uint32_t _retryTimeout;
        time_t _initTime = 0;
        String _ssid;
        String _password;
        String _hostName;
        WiFiNTP& _timeServer;
        WebServer& _webServer;
        Log<const char>& _eventLog;
        void (*_handlers[10])(void); // function pointers indexed by state

        void setState(WiFiState newState);
        void blinkLED(int freq);
        String getResetReason();
};

#endif