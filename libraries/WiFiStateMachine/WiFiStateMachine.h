#ifndef WIFI_STATE_MACHINE_H
#define WIFI_STATE_MACHINE_H

#include <stdint.h>
#include <ESPWebServer.h>
#include <WiFiNTP.h>
#include <Log.h>

enum struct WiFiInitState
{
    Booting = 0,
    Initializing = 1,
    AwaitingConnection = 2,
    Connecting = 3,
    ConnectFailed = 4,
    Connected = 5,
    TimeServerInitializing = 6,
    TimeServerSyncing = 7,
    TimeServerSyncFailed = 8,
    TimeServerSynced = 9,
    Initialized = 10,
    Updating = 11
};


class WiFiStateMachine
{
    public:
        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, Log<const char>& eventLog);

        void on(WiFiInitState state, void (*handler)(void));
 
        void begin(String ssid, String password, String hostName);
        void run();
        void reset();

        void logEvent(String msg);
        time_t getCurrentTime();
        bool shouldPerformAction(String name);

        time_t inline getInitTime()
        {
            return _initTime;
        }

        uint32_t inline getUptime()
        {
            return getCurrentTime() - _initTime;
        }

        WiFiInitState inline getState()
        {
            return _state;
        }

        bool inline isInAccessPointMode()
        {
            return _isInAccessPointMode;
        }

        String inline getIPAddress()
        {
            return _ipAddress.toString();
        }

    private:
        WiFiInitState _state = WiFiInitState::Booting;
        uint32_t _stateChangeTime = 0;
        uint32_t _retryTimeout;
        uint32_t _resetTime = 0;
        time_t _initTime = 0;
        time_t _actionPerformedTime = 0;
        String _ssid;
        String _password;
        String _hostName;
        WiFiNTP& _timeServer;
        ESPWebServer& _webServer;
        Log<const char>& _eventLog;
        void (*_handlers[static_cast<int>(WiFiInitState::Updating) + 1])(void); // function pointers indexed by state
        bool _isTimeServerAvailable = false;
        bool _isInAccessPointMode = false;
        IPAddress _ipAddress;

        void initializeAP();
        void initializeSTA();
        void setState(WiFiInitState newState, bool callHandler = false);
        void blinkLED(int tOn, int tOff);
        String getResetReason();
};

#endif