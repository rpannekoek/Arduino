#include "WiFiStateMachine.h"
#include <Arduino.h>
#include <ESPWiFi.h>
#include <Tracer.h>

#ifdef ESP32
#include <rom/rtc.h>
#endif


// Constructor
WiFiStateMachine::WiFiStateMachine(WiFiNTP& timeServer, WebServer& webServer, Log<const char>& eventLog)
    : _timeServer(timeServer), _webServer(webServer), _eventLog(eventLog)
{
    memset(_handlers, 0, sizeof(_handlers));
}


void WiFiStateMachine::on(WiFiState state, void (*handler)(void))
{
    _handlers[static_cast<int>(state)] = handler;
}


void WiFiStateMachine::begin(String ssid, String password, String hostName)
{
    Tracer tracer(F("WiFiStateMachine::begin"), hostName.c_str());

    _ssid = ssid;
    _password = password;
    _hostName = hostName;

    String event = "Booted from ";
    event += getResetReason();
    logEvent(event);
    
    setState(WiFiState::Initializing);
}


time_t WiFiStateMachine::getCurrentTime()
{
    if (_state == WiFiState::Initialized)
        return _timeServer.getCurrentTime();
    else
        return millis() / 1000;
}


void WiFiStateMachine::logEvent(String msg)
{
    Tracer tracer(F("WiFiStateMachine::logEvent"), msg.c_str());

    size_t timestamp_size = 23; // strlen("2019-01-30 12:23:34 : ") + 1;

    char* event = new char[timestamp_size + msg.length()];

    if (_state == WiFiState::Initialized)
    {
        time_t currentTime = _timeServer.getCurrentTime();
        strftime(event, timestamp_size, "%F %H:%M:%S : ", gmtime(&currentTime));
    }
    else
        snprintf(event, timestamp_size, "@ %u ms : ", static_cast<uint32_t>(millis()));
    
    strcat(event, msg.c_str());

    _eventLog.add(event);

    TRACE(F("%u event log entries\n"), _eventLog.count());
}


void WiFiStateMachine::setState(WiFiState newState)
{
    _state = newState;
    _stateChangeTime = millis();
    TRACE(F("WiFi state: %u @ %u ms\n"), _state, _stateChangeTime);
}


void WiFiStateMachine::run()
{
    uint32_t currentMillis = millis();
    String event;

    // First trigger custom handler (if any)
    int state = static_cast<int>(_state);
    if (_handlers[state] != nullptr)
        _handlers[state]();

    switch (_state)
    {
        case WiFiState::Initializing:
            TRACE(F("Connecting to WiFi network '%s' ...\n"), _ssid.c_str());
            WiFi.mode(WIFI_STA);
#ifdef ESP8266
            WiFi.hostname(_hostName);
#else
            WiFi.setHostname(_hostName.c_str());
#endif
            WiFi.setAutoReconnect(true);
            WiFi.disconnect();
            WiFi.begin(_ssid.c_str(), _password.c_str());
            setState(WiFiState::Connecting);
            break;

        case WiFiState::Connecting:
            if (WiFi.status() == WL_CONNECTED)
                setState(WiFiState::Connected);
            else if (currentMillis >= (_stateChangeTime + 15000))
            {
                TRACE(F("Timeout connecting WiFi\n"));
                setState(WiFiState::ConnectFailed);
            }
            break;

        case WiFiState::ConnectFailed:
            // Retry WiFi initialization after 60 seconds
            if (currentMillis >= (_stateChangeTime + 60000))
                setState(WiFiState::Initializing);
            else
                blinkLED(2);
            break;

        case WiFiState::Connected:
            event = F("WiFi connected. IP address: ");
            event += WiFi.localIP().toString();
            logEvent(event);
            _webServer.begin();
            setState(WiFiState::TimeServerInitializing);
            break;

        case WiFiState::TimeServerInitializing:
            if (_timeServer.beginGetServerTime())
                setState(WiFiState::TimeServerSyncing);
            else
                setState(WiFiState::TimeServerSyncFailed);
            break;

        case WiFiState::TimeServerSyncing:
            _initTime = _timeServer.endGetServerTime(); 
            if (_initTime == 0)
            {
                // Timeout after 5 seconds
                if (currentMillis >= (_stateChangeTime + 5000))
                {
                    TRACE(F("Timeout waiting for NTP server response\n"));
                    setState(WiFiState::TimeServerSyncFailed);
                }
            }
            else
                setState(WiFiState::TimeServerSynced);
            break;
        
        case WiFiState::TimeServerSyncFailed:
            // Retry Time Server sync after 15 seconds
            if (currentMillis >= (_stateChangeTime + 15000))
                setState(WiFiState::TimeServerInitializing);
            else
                blinkLED(3);
            break;

        case WiFiState::TimeServerSynced:
            logEvent(F("Time synchronized"));
            setState(WiFiState::Initialized);
            break;

        default:
            // Nothing to do
            break;
    }

    if (_state > WiFiState::Connected)
        _webServer.handleClient();
}


void WiFiStateMachine::blinkLED(int freq)
{
    int interval = 500 / freq;
    digitalWrite(LED_BUILTIN, 0);
    delay(interval);
    digitalWrite(LED_BUILTIN, 1);
    delay(interval);
}


String WiFiStateMachine::getResetReason()
{
#ifdef ESP8266
    return ESP.getResetReason();
#else
    String result;
    switch (rtc_get_reset_reason(0))
    {
        case 1  : result = "Power on reset"; break;
        case 3  : result = "Software reset"; break;
        case 4  : result = "Legacy watch dog reset"; break;
        case 5  : result = "Deep Sleep reset"; break;
        case 6  : result = "Reset by SLC module"; break;
        case 7  : result = "Timer Group 0 Watch dog reset"; break;
        case 8  : result = "Timer Group 1 Watch dog reset"; break;
        case 9  : result = "RTC Watch dog reset"; break;
        case 10 : result = "Instrusion tested to reset CPU"; break;
        case 11 : result = "Time Group reset CPU"; break;
        case 12 : result = "Software reset CPU"; break;
        case 13 : result = "RTC Watch dog Reset CPU"; break;
        case 14 : result = "APP CPU reset by PRO CPU"; break;
        case 15 : result = "Brownout (voltage is not stable)"; break;
        case 16 : result = "RTC Watch dog reset digital core and rtc module"; break;
        default : result = "Unknown";
    }
    return result;
#endif
}
