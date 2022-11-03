#include "WiFiStateMachine.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPWiFi.h>
#include <ESPFileSystem.h>
#include <Tracer.h>

#ifdef ESP32
    #include <rom/rtc.h>
#else
    #define U_SPIFFS U_FS
#endif

#define CONNECT_TIMEOUT_MS 10000
#define NTP_TIMEOUT_MS 5000
#define NTP_RETRY_INTERVAL_MS 10000
#define MAX_RETRY_INTERVAL_MS 300000U

bool WiFiStateMachine::_staDisconnected = false;


// Constructor
WiFiStateMachine::WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, Log<const char>& eventLog)
    : _timeServer(timeServer), _webServer(webServer)
{
    _eventLogPtr = &eventLog;
    _eventStringLogPtr = nullptr;
    memset(_handlers, 0, sizeof(_handlers));
}

// Constructor
WiFiStateMachine::WiFiStateMachine(WiFiNTP& timeServer, ESPWebServer& webServer, StringLog& eventLog)
    : _timeServer(timeServer), _webServer(webServer)
{
    _eventStringLogPtr = &eventLog;
    _eventLogPtr = nullptr;
    memset(_handlers, 0, sizeof(_handlers));
}


void WiFiStateMachine::on(WiFiInitState state, void (*handler)(void))
{
    _handlers[static_cast<int>(state)] = handler;
}


void WiFiStateMachine::begin(String ssid, String password, String hostName, uint32_t reconnectInterval)
{
    Tracer tracer(F("WiFiStateMachine::begin"), hostName.c_str());

    _reconnectInterval = reconnectInterval * 1000;
    _ssid = ssid;
    _password = password;
    _hostName = hostName;
    _retryInterval = 5000; // Start exponential backoff with 5 seconds
    _isTimeServerAvailable = false;
    _resetTime = 0;

    logEvent(F("Booted from %s"), getResetReason().c_str());

    ArduinoOTA.onStart(
        [this]() 
        {
            TRACE(F("OTA start %d\n"), ArduinoOTA.getCommand());
            if (ArduinoOTA.getCommand() == U_SPIFFS)
                SPIFFS.end();
            setState(WiFiInitState::Updating, true);
        });

    ArduinoOTA.onEnd(
        [this]()
        {
            TRACE(F("OTA end %d\n"), ArduinoOTA.getCommand());
            setState(WiFiInitState::Initialized);
        });

    ArduinoOTA.onError(
        [this](ota_error_t error)
        {
            TRACE(F("OTA error %u\n"), error);
            setState(WiFiInitState::Initialized);
        });

#ifdef ESP8266
    _staDisconnectedEvent = WiFi.onStationModeDisconnected(&WiFiStateMachine::onStationDisconnected);
#elif defined(ESP32V1)
    _staDisconnectedEvent = WiFi.onEvent(WiFiStateMachine::onStationDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);
#else
    _staDisconnectedEvent = WiFi.onEvent(WiFiStateMachine::onStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

    setState(WiFiInitState::Initializing);
}


time_t WiFiStateMachine::getCurrentTime()
{
    if (_isTimeServerAvailable)
        return _timeServer.getCurrentTime();
    else
        return millis() / 1000;
}


void WiFiStateMachine::logEvent(String format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(_logMessage, sizeof(_logMessage), format.c_str(), args);
    va_end(args);

    _logMessage[sizeof(_logMessage)-1] = 0; // Ensure the string is always null-terminated

    logEvent(_logMessage);
}


void WiFiStateMachine::logEvent(const char* msg)
{
    Tracer tracer(F("WiFiStateMachine::logEvent"), msg);

    size_t timestamp_size = 23; // strlen("2019-01-30 12:23:34 : ") + 1;

    char* event = new char[timestamp_size + strlen(msg)];

    if (_isTimeServerAvailable)
    {
        time_t currentTime = _timeServer.getCurrentTime();
        strftime(event, timestamp_size, "%F %H:%M:%S : ", localtime(&currentTime));
    }
    else
        snprintf(event, timestamp_size, "@ %u ms : ", static_cast<uint32_t>(millis()));
    
    strcat(event, msg);

    if (_eventStringLogPtr == nullptr)
    {
        _eventLogPtr->add(event);
        TRACE(F("%u event log entries\n"), _eventLogPtr->count());
    }
    else
    {
        _eventStringLogPtr->add(event);
        delete[] event;
        TRACE(F("%u event log entries\n"), _eventStringLogPtr->count());
    }
}


void WiFiStateMachine::setState(WiFiInitState newState, bool callHandler)
{
    uint32_t prevStateChangeTime = _stateChangeTime;
    _stateChangeTime = millis();
    TRACE(
        F("WiFi state: %d -> %d @ +%u ms\n"),
        _state,
        newState,
        _stateChangeTime - prevStateChangeTime);

    _state = newState;
    if (callHandler)
    {
        int state = static_cast<int>(_state);
        if (_handlers[state] != nullptr)
            _handlers[state]();
    }
}


void WiFiStateMachine::initializeAP()
{
    TRACE(F("Starting WiFi network '%s' ...\n"), _hostName.c_str());

    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP))
        TRACE(F("Unable to set WiFi mode\n"));

    if (!WiFi.softAP(_hostName.c_str()))
        TRACE(F("Unable to start Access Point\n"));

    _ipAddress = WiFi.softAPIP();
    logEvent(F("Started Access Point mode. IP address: %s"), getIPAddress().c_str());
}


void WiFiStateMachine::initializeSTA()
{
    TRACE(F("Connecting to WiFi network '%s' ...\n"), _ssid.c_str());
    WiFi.persistent(false);
    if (!WiFi.setAutoReconnect(_reconnectInterval == 0))
        TRACE(F("Unable to set auto reconnect\n"));
#ifdef ESP8266
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.disconnect())
        TRACE(F("WiFi disconnect failed\n"));
    if (!WiFi.hostname(_hostName))
        TRACE(F("Unable to set host name\n"));
#elif defined(ESP32V1)
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.disconnect())
        TRACE(F("WiFi disconnect failed\n"));
    // ESP32 doesn't reliably set the status to WL_DISCONNECTED despite disconnect() call.
    WiFiSTAClass::_setStatus(WL_DISCONNECTED);
    // See https://github.com/espressif/arduino-esp32/issues/2537
    IPAddress none = IPAddress(0,0,0,0);
    if (!WiFi.config(none, none, none))
        TRACE(F("WiFi.config failed\n"));
    if (!WiFi.setHostname(_hostName.c_str()))
        TRACE(F("Unable to set host name ('%s')\n"), _hostName.c_str());
#else
    if (!WiFi.mode(WIFI_MODE_NULL))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.setHostname(_hostName.c_str()))
        TRACE(F("Unable to set host name ('%s')\n"), _hostName.c_str());
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.disconnect())
        TRACE(F("WiFi disconnect failed\n"));
    // ESP32 doesn't reliably set the status to WL_DISCONNECTED despite disconnect() call.
    WiFiSTAClass::_setStatus(WL_DISCONNECTED);
#endif
    ArduinoOTA.setHostname(_hostName.c_str());
    _staDisconnected = false;
    WiFi.begin(_ssid.c_str(), _password.c_str());
}


void WiFiStateMachine::run()
{
    uint32_t currentMillis = millis();
    uint32_t currentStateMillis = currentMillis - _stateChangeTime;
    wl_status_t wifiStatus = WiFi.status();
    String event;

    // First trigger custom handler (if any)
    void (*handler)(void) = _handlers[static_cast<int>(_state)]; 
    if (handler != nullptr) handler();

    switch (_state)
    {
        case WiFiInitState::Initializing:
            if (_ssid.length() == 0)
            {
                initializeAP();
                _isInAccessPointMode = true;
                setState(WiFiInitState::AwaitingConnection);
            }
            else
            {
                initializeSTA();
                _isInAccessPointMode = false;
                setState(WiFiInitState::Connecting);
            }
            TRACE(F("WiFi status: %d\n"), WiFi.status());
            break;

        case WiFiInitState::AwaitingConnection:
            if (WiFi.softAPgetStationNum() > 0)
            {
                _webServer.begin();
                // Skip actual time server sync (no internet access), but still trigger TimeServerSynced event.
                setState(WiFiInitState::TimeServerSynced);
            }
            else
                blinkLED(400, 100);
            break;

        case WiFiInitState::Connecting:
            if (wifiStatus == WL_CONNECTED)
                setState(WiFiInitState::Connected);
            else if (wifiStatus == WL_CONNECT_FAILED)
                setState(WiFiInitState::ConnectFailed); 
            else if (currentStateMillis >= CONNECT_TIMEOUT_MS)
            {
                TRACE(F("Timeout connecting WiFi.\n"));
                setState(WiFiInitState::ConnectFailed);
            }
            break;

        case WiFiInitState::Reconnecting:
            if (wifiStatus == WL_CONNECTED)
            {
                logEvent(F("WiFi reconnected"));
                setState(WiFiInitState::Initialized);
            }
            else if (_staDisconnected || (wifiStatus == WL_NO_SSID_AVAIL) || (currentStateMillis >= CONNECT_TIMEOUT_MS))
            {
                TRACE(F("Reconnecting WiFi failed. Status: %d\n"), wifiStatus);
#ifdef ESP8266
                if (!WiFi.forceSleepBegin())
                    TRACE(F("forceSleepBegin() failed.\n"));
#endif
                setState(WiFiInitState::ConnectionLost);
            }
            else
            {
                // Still also trigger Initialized handler (for backwards compatibility)
                void (*initHandler)(void) = _handlers[static_cast<int>(WiFiInitState::Initialized)]; 
                if (initHandler != nullptr) initHandler();
            }
            break;

        case WiFiInitState::ConnectionLost:
            if (wifiStatus == WL_CONNECTED)
            {
                logEvent(F("WiFi reconnected"));
                setState(WiFiInitState::Initialized);
            }
            else if ((_reconnectInterval != 0) && (currentStateMillis >= _reconnectInterval))
            {
                TRACE(F("Attempting WiFi reconnect...\n"));
                _staDisconnected = false;
#ifdef ESP8266
                if (!WiFi.forceSleepWake())
                    TRACE(F("forceSleepWake() failed.\n"));
#else
                // ESP32 doesn't reliably set status:
                WiFiSTAClass::_setStatus(WL_CONNECTION_LOST);
                if (!WiFi.reconnect())
                    TRACE(F("reconnect() failed.\n"));
#endif
                TRACE(F("WiFi status: %d\n"), WiFi.status());
                setState(WiFiInitState::Reconnecting);
            }
            else
            {
                // Still also trigger Initialized handler (for backwards compatibility)
                void (*initHandler)(void) = _handlers[static_cast<int>(WiFiInitState::Initialized)]; 
                if (initHandler != nullptr) initHandler();
            }
            break;

        case WiFiInitState::ConnectFailed:
            if (currentStateMillis >= _retryInterval)
            {
                // Exponential backoff
                _retryInterval = std::min(_retryInterval * 2, MAX_RETRY_INTERVAL_MS);
                setState(WiFiInitState::Initializing);
            }
            else
                blinkLED(500, 500);
            break;

        case WiFiInitState::Connected:
#ifdef DEBUG_ESP_PORT
            WiFi.printDiag(DEBUG_ESP_PORT);
#endif
            _ipAddress = WiFi.localIP();
            logEvent(F("WiFi connected. IP address: %s"), getIPAddress().c_str());
            ArduinoOTA.begin();
            _webServer.begin();
            setState(WiFiInitState::TimeServerInitializing);
            break;

        case WiFiInitState::TimeServerInitializing:
            if (_timeServer.beginGetServerTime())
                setState(WiFiInitState::TimeServerSyncing);
            else
                setState(WiFiInitState::TimeServerSyncFailed);
            break;

        case WiFiInitState::TimeServerSyncing:
            _initTime = _timeServer.endGetServerTime(); 
            if (_initTime == 0)
            {
                if (currentStateMillis >= NTP_TIMEOUT_MS)
                {
                    TRACE(F("Timeout waiting for NTP server response\n"));
                    setState(WiFiInitState::TimeServerSyncFailed);
                }
            }
            else
            {
                _isTimeServerAvailable = true;
                setState(WiFiInitState::TimeServerSynced);
            }
            break;
        
        case WiFiInitState::TimeServerSyncFailed:
            if (currentStateMillis >= NTP_RETRY_INTERVAL_MS)
                setState(WiFiInitState::TimeServerInitializing);
            else
                blinkLED(250, 250);
            break;

        case WiFiInitState::TimeServerSynced:
            if (_isTimeServerAvailable)
            {
                logEvent(F("Time synchronized using NTP server: %s"), _timeServer.NTPServer);
            }
            setState(WiFiInitState::Initialized);
            break;

        case WiFiInitState::Initialized:
            if (!_isInAccessPointMode && (_staDisconnected || (wifiStatus != WL_CONNECTED)))
            {
                logEvent(F("WiFi connection lost"));
                TRACE(F("WiFi status: %d\n"), wifiStatus);
                if (_reconnectInterval != 0)
                {
#ifdef ESP8266
                    if (!WiFi.forceSleepBegin())
                        TRACE(F("forceSleepBegin() failed.\n"));
#endif
                }
                setState(WiFiInitState::ConnectionLost);
            }
            break;

        default:
            // Nothing to do
            break;
    }

    // Automatic Modem sleep leverages delay() to reduce power
    if (_state > WiFiInitState::Connected)
    {
        _webServer.handleClient();
        ArduinoOTA.handle();
        delay(10);
    }
    else
        delay(100);

    if ((_resetTime > 0) && (currentMillis >= _resetTime))
    {
        TRACE(F("Resetting...\n"));
        ESP.restart();
        // From non-OS SDK Reference:
        // The ESP8266 will not restart immediately. Please do not call other functions after calling this API. 
        delay(1000); 
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


bool WiFiStateMachine::shouldPerformAction(String name)
{
    if (!_webServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = _webServer.arg(name).toInt();

    if (actionTime == _actionPerformedTime)
        return false; // Action already performed

    _actionPerformedTime = actionTime;
    return true;
}


#ifdef ESP8266
void WiFiStateMachine::onStationDisconnected(const WiFiEventStationModeDisconnected& evt)
{
    TRACE(F("STA disconnected. Reason: %d\n"), evt.reason);
    _staDisconnected = true;
}
#elif defined(ESP32V1)
void WiFiStateMachine::onStationDisconnected(system_event_id_t event, system_event_info_t info)
{
    TRACE(F("STA disconnected. Reason: %d\n"), info.disconnected.reason);
    _staDisconnected = true;
}
#else
void WiFiStateMachine::onStationDisconnected(arduino_event_id_t event, arduino_event_info_t info)
{
    TRACE(F("STA disconnected. Reason: %d\n"), info.wifi_sta_disconnected.reason);
    _staDisconnected = true;
}
#endif
