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
    ConnectionLost = 5,
    Reconnecting = 6,
    Connected = 7,
    TimeServerInitializing = 8,
    TimeServerSyncing = 9,
    TimeServerSyncFailed = 10,
    TimeServerSynced = 11,
    Initialized = 12,
    Updating = 13
};


class WiFiStateMachine
{
    public:
        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, Log<const char>& eventLog);

        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, StringLog& eventLog);

        void on(WiFiInitState state, void (*handler)(void));
 
        void begin(String ssid, String password, String hostName, uint32_t reconnectInterval = 60);
        void run();
        void reset();

        void logEvent(String format, ...);
        void logEvent(const char* msg);
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

        bool inline isConnected()
        {
            return _state >= WiFiInitState::Connected;
        }

    private:
        WiFiInitState _state = WiFiInitState::Booting;
        static bool _staDisconnected;
        uint32_t _reconnectInterval = 0;
        uint32_t _stateChangeTime = 0;
        uint32_t _retryInterval;
        uint32_t _resetTime = 0;
        time_t _initTime = 0;
        time_t _actionPerformedTime = 0;
        String _ssid;
        String _password;
        String _hostName;
        WiFiNTP& _timeServer;
        ESPWebServer& _webServer;
        Log<const char>* _eventLogPtr;
        StringLog* _eventStringLogPtr;
        void (*_handlers[static_cast<int>(WiFiInitState::Updating) + 1])(void); // function pointers indexed by state
        bool _isTimeServerAvailable = false;
        bool _isInAccessPointMode = false;
        IPAddress _ipAddress;
        char _logMessage[64];

        void initializeAP();
        void initializeSTA();
        void setState(WiFiInitState newState, bool callHandler = false);
        void blinkLED(int tOn, int tOff);
        String getResetReason();
        
#ifdef ESP8266
        WiFiEventHandler _staDisconnectedEvent; 
        static void onStationDisconnected(const WiFiEventStationModeDisconnected& evt);
#elif defined(ESP32V1)
        wifi_event_id_t _staDisconnectedEvent;
        static void onStationDisconnected(system_event_id_t event, system_event_info_t info);
#else
        wifi_event_id_t _staDisconnectedEvent;
        static void onStationDisconnected(arduino_event_id_t event, arduino_event_info_t info);
#endif
};

#endif