#ifndef WIFI_STATE_MACHINE_H
#define WIFI_STATE_MACHINE_H

#include <c_types.h>
#include <ESP8266WebServer.h>
#include <WiFiNTP.h>
#include <Log.h>


typedef enum
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
} WiFiState;


class WiFiStateMachine
{
    public:
        // Constructor
        WiFiStateMachine(WiFiNTP& timeServer, ESP8266WebServer& webServer, Log& eventLog);

        void on(WiFiState state, void (*handler)(void));
 
        void begin(String ssid, String password, String hostName);
        void run();

        void logEvent(String msg);
        time_t getCurrentTime();

        WiFiState getState()
        {
            return _state;
        }

    protected:
        WiFiState _state = WiFiState::Booting;
        uint32_t _stateChangeTime = 0;
        String _ssid;
        String _password;
        String _hostName;
        WiFiNTP& _timeServer;
        ESP8266WebServer& _webServer;
        Log& _eventLog;
        void (*_handlers[8])(void); // function pointers indexed by state

        void setState(WiFiState newState);
        void blinkLED(int freq);
};

#endif