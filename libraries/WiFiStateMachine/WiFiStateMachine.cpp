#include "WiFiStateMachine.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPWiFi.h>
#include <ESPFileSystem.h>
#include <Tracer.h>

#ifdef ESP32
#include <rom/rtc.h>
#endif

#define MAX_RETRY_TIMEOUT 300000

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
    _retryTimeout = 5000; // Start exponential backoff with 5 seconds
    _isTimeServerAvailable = false;
    _resetTime = 0;

    String event = "Booted from ";
    event += getResetReason();
    logEvent(event);

    ArduinoOTA.onStart([]() 
        {
            TRACE(F("OTA start %d\n"), ArduinoOTA.getCommand());
            if (ArduinoOTA.getCommand() == U_SPIFFS)
                SPIFFS.end();
        });
    ArduinoOTA.onEnd([]() { TRACE(F("OTA end %d\n"), ArduinoOTA.getCommand()); });
    ArduinoOTA.onError([](ota_error_t error) { TRACE(F("OTA error %u\n"), error); });
    
    setState(WiFiState::Initializing);
}


time_t WiFiStateMachine::getCurrentTime()
{
    if (_isTimeServerAvailable)
        return _timeServer.getCurrentTime();
    else
        return millis() / 1000;
}


void WiFiStateMachine::logEvent(String msg)
{
    Tracer tracer(F("WiFiStateMachine::logEvent"), msg.c_str());

    size_t timestamp_size = 23; // strlen("2019-01-30 12:23:34 : ") + 1;

    char* event = new char[timestamp_size + msg.length()];

    if (_isTimeServerAvailable)
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


void WiFiStateMachine::initializeAP()
{
    TRACE(F("Starting WiFi network '%s' ...\n"), AP_SSID);

    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP))
        TRACE(F("Unable to set WiFi mode\n"));

    if (!WiFi.softAP(AP_SSID))
        TRACE(F("Unable to start Access Point\n"));

    String event = F("Started Access Point mode. IP address: ");
    event += WiFi.softAPIP().toString();
    logEvent(event);
}


void WiFiStateMachine::initializeSTA()
{
    TRACE(F("Connecting to WiFi network '%s' ...\n"), _ssid.c_str());
    WiFi.persistent(false);
    if (!WiFi.setAutoReconnect(true))
        TRACE(F("Unable to set auto reconnect\n"));
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.disconnect())
        TRACE(F("WiFi disconnect failed\n"));
#ifdef ESP8266
    if (!WiFi.hostname(_hostName))
        TRACE(F("Unable to set host name\n"));
#else
    if (!WiFi.setHostname(_hostName.c_str()))
        TRACE(F("Unable to set host name ('%s')\n"), _hostName.c_str());
#endif
    ArduinoOTA.setHostname(_hostName.c_str());
    WiFi.begin(_ssid.c_str(), _password.c_str());
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
            if (_ssid.length() == 0)
            {
                initializeAP();
                _isInAccessPointMode = true;
                setState(WiFiState::AwaitingConnection);
            }
            else
            {
                initializeSTA();
                _isInAccessPointMode = false;
                setState(WiFiState::Connecting);
            }
            break;

        case WiFiState::AwaitingConnection:
            if (WiFi.softAPgetStationNum() > 0)
            {
                _webServer.begin();
                // Skip actual time server sync (no internet access), but still trigger TimeServerSynced event.
                setState(WiFiState::TimeServerSynced);
            }
            else
                blinkLED(400, 100);
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
            if (currentMillis >= (_stateChangeTime + _retryTimeout))
            {
                _retryTimeout *= 2; // Exponential backoff
                if (_retryTimeout > MAX_RETRY_TIMEOUT) _retryTimeout = MAX_RETRY_TIMEOUT;
                setState(WiFiState::Initializing);
            }
            else
                blinkLED(500, 500);
            break;

        case WiFiState::Connected:
            event = F("WiFi connected. IP address: ");
            event += WiFi.localIP().toString();
            logEvent(event);
            ArduinoOTA.begin();
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
            {
                _isTimeServerAvailable = true;
                setState(WiFiState::TimeServerSynced);
            }
            break;
        
        case WiFiState::TimeServerSyncFailed:
            // Retry Time Server sync after 15 seconds
            if (currentMillis >= (_stateChangeTime + 15000))
                setState(WiFiState::TimeServerInitializing);
            else
                blinkLED(250, 250);
            break;

        case WiFiState::TimeServerSynced:
            if (_isTimeServerAvailable)
            {
                String event = F("Time synchronized using NTP server: ");
                event += _timeServer.NTPServer; 
                logEvent(event);
            }
            setState(WiFiState::Initialized);
            break;

        default:
            // Nothing to do
            break;
    }

    if (_state > WiFiState::Connected)
    {
        _webServer.handleClient();
        ArduinoOTA.handle();
    }

    if ((_resetTime > 0) && (currentMillis >= _resetTime))
    {
        TRACE(F("Resetting...\n"));
        ESP.restart();
    }
}


void WiFiStateMachine::reset()
{
    _resetTime = millis() + 1000;
}

void WiFiStateMachine::blinkLED(int tOn, int tOff)
{
    digitalWrite(LED_BUILTIN, 0);
    delay(tOn);
    digitalWrite(LED_BUILTIN, 1);
    delay(tOff);
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
