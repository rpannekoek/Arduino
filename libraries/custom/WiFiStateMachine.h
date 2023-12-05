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
    SwitchingAP = 6,
    Reconnecting = 7,
    Connected = 8,
    TimeServerInitializing = 9,
    TimeServerSyncing = 10,
    TimeServerSyncFailed = 11,
    TimeServerSynced = 12,
    Initialized = 13,
    Updating = 14
};


class WiFiStateMachine
{
    public:
        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, Log<const char>& eventLog);

        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, StringLog& eventLog);

        void on(WiFiInitState state, void (*handler)(void));

        void registerStaticFiles(PGM_P* files, size_t count);
 
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

        void scanAccessPoints(uint32_t intervalSeconds = 1800, uint32_t switchDelaySeconds = 1800, int8_t rssiThreshold = 6)
        {
            _scanAccessPointsInterval = intervalSeconds;
            _switchAccessPointDelay = switchDelaySeconds;
            _rssiThreshold = rssiThreshold;
        }

    private:
        WiFiInitState _state = WiFiInitState::Booting;
        static bool _staDisconnected;
        uint32_t _reconnectInterval = 0;
        uint32_t _stateChangeTime = 0;
        time_t _scanAccessPointsTime = 0;
        uint32_t _scanAccessPointsInterval = 0;
        uint32_t _switchAccessPointDelay;
        int8_t _rssiThreshold;
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
        void scanForBetterAccessPoint();
        
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