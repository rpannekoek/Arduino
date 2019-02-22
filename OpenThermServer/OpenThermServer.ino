#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <OTGW.h>
#include <Log.h>
#include <WeatherAPI.h>
#include "PersistentData.h"
#include "OpenThermLogEntry.h"
#include "WiFiCredentials.private.h"


#define NTP_SERVER "fritz.box"
#define FTP_SERVER "fritz.box"
#define ICON "/apple-touch-icon.png"
#define WATCHDOG_INTERVAL_MS 1000
#define OTGW_STARTUP_TIME 5
#define OTGW_TIMEOUT 60
#define HTTP_POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define EVENT_LOG_LENGTH 100
#define OT_LOG_LENGTH 240
#define KEEP_TSET_LOW_DURATION 10*60
#define WEATHER_SERVICE_POLL_INTERVAL 15*60
#define WEATHER_SERVICE_RESPONSE_TIMEOUT 10
#define FTP_RETRY_INTERVAL 60*60

const char* BOILER_LEVEL_NAMES[5] = {"Off", "Low", "Medium", "High", "Thermostat"};

typedef enum
{
    Off,
    Low,
    Medium,
    High,
    Thermostat
} BoilerLevel;


OpenThermGateway OTGW(Serial, 14);
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, 24 * 3600); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
WeatherAPI WeatherService(2000); // 2 sec request timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
Log EventLog(EVENT_LOG_LENGTH);
Log OpenThermLog(OT_LOG_LENGTH);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

// OpenTherm data values indexed by data ID
uint16_t thermostatRequests[256];
uint16_t boilerResponses[256];
uint16_t otgwRequests[256];
uint16_t otgwResponses[256];

uint32_t watchdogFeedTime = 0;
time_t initTime = 0;
time_t currentTime = 0;
time_t otLogTime = 0;
time_t otgwInitializeTime = OTGW_STARTUP_TIME;
time_t otgwTimeout = OTGW_TIMEOUT;
time_t weatherServicePollTime = 0;
time_t weatherServiceTimeout = 0;
time_t lastWeatherUpdateTime = 0;

int lastWeatherResult = 0;
bool updateTOutside = false;

OpenThermLogEntry* lastOTLogEntryPtr = NULL;
uint16_t otLogEntriesToSync = 0;
time_t otLogSyncTime = 0;
time_t lastOTLogSyncTime = 0;

char stringBuffer[128];

int boilerTSet[5] = {0, 40, 50, 60, 0}; // TODO: configurable

BoilerLevel currentBoilerLevel = BoilerLevel::Thermostat;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime = 0;
time_t boilerOverrideStartTime = 0;
uint32_t totalOverrideDuration = 0;


int formatTime(char* output, size_t output_size, const char* format, time_t time)
{
    return strftime(output, output_size, format, gmtime(&time));
}


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


// Boot code
void setup() 
{
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    Serial.begin(9600);
    Serial.setTimeout(1000);
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log", handleHttpOpenThermLogRequest);
    WebServer.on("/log/sync", handleHttpOpenThermLogSyncRequest);
    WebServer.on("/log-csv", handleHttpOpenThermLogCsvRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/cmd", HTTP_GET, handleHttpCommandFormRequest);
    WebServer.on("/cmd", HTTP_POST, handleHttpCommandFormPost);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    memset(stringBuffer, 0, sizeof(stringBuffer));

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));
    memset(otgwRequests, 0xFF, sizeof(otgwRequests));
    memset(otgwResponses, 0xFF, sizeof(otgwResponses));

    WiFiSM.on(WiFiState::TimeServerInitializing, onTimeServerInit);
    WiFiSM.on(WiFiState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiState::Initialized, onWiFiInitialized);
    WiFiSM.begin(WIFI_SSID, WIFI_PASSWORD, PersistentData.hostName);

    String event = "Booted from ";
    event += ESP.getResetReason();
    logEvent(event);

    Tracer::traceFreeHeap();

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    if (millis() >= watchdogFeedTime)
    {
        OTGW.feedWatchdog();
        watchdogFeedTime = millis() + WATCHDOG_INTERVAL_MS;
    }

    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, 0);
        handleSerialData();
        otgwTimeout = currentTime + OTGW_TIMEOUT;
        digitalWrite(LED_BUILTIN, 1);
        return;
    }

    if (currentTime >= otgwTimeout)
    {
        logEvent(F("OTGW Timeout"));
        resetOpenThermGateway();
        return;
    }

    if ((otgwInitializeTime != 0) && (currentTime >= otgwInitializeTime))
    {
        otgwInitializeTime = 0;
        initializeOpenThermGateway();
        return;
    }

    // Scheduled Boiler TSet change
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        changeBoilerLevelTime = 0;
        setBoilerLevel(changeBoilerLevel);
        return;
    }

    if (updateTOutside)
    {
        updateTOutside = false;
        setTOutside(WeatherService.temperature);
        return;
    }

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();

    delay(10);
}


void onTimeServerInit()
{
    // Time server initialization (DNS lookup) make take a few seconds
    // Feed OTGW watchdog just before to prevent getting bitten
    OTGW.feedWatchdog();
}


void onTimeServerSynced()
{
    // After time server sync all times jump ahead from 1-1-1970
    initTime = WiFiSM.getCurrentTime();
    time_t timeJump = initTime - currentTime;
    currentTime = initTime;
    otgwTimeout += timeJump;
    boilerOverrideStartTime += timeJump;
    if (otgwInitializeTime != 0) 
        otgwInitializeTime += timeJump;
    if (changeBoilerLevelTime != 0) 
        changeBoilerLevelTime += timeJump;
    if (boilerOverrideStartTime != 0)
        boilerOverrideStartTime += timeJump;
}


void onWiFiInitialized()
{
    // Get outside temperature from Weather Service
    if (currentTime >= weatherServicePollTime)
    {
        weatherServicePollTime = currentTime + WEATHER_SERVICE_POLL_INTERVAL;

        const char* apiKey = PersistentData.weatherApiKey;
        if (apiKey[0] != 0)
        {
            OTGW.feedWatchdog();
            if (WeatherService.beginRequestData(apiKey, PersistentData.weatherLocation))
                weatherServiceTimeout = currentTime + WEATHER_SERVICE_RESPONSE_TIMEOUT;
            else
                TRACE(F("Failed sending request to weather service\n"));
            return;
        }
    }

    if (weatherServiceTimeout != 0)
    {
        if (currentTime >= weatherServiceTimeout)
        {
            TRACE(F("Timeout waiting for weather service response\n"));
            WeatherService.close();
            lastWeatherResult = WEATHER_ERROR_TIMEOUT;
            weatherServiceTimeout = 0;
            return;
        }

        int httpCode = WeatherService.endRequestData();
        if (httpCode != 0)
        {
            weatherServiceTimeout = 0;
            lastWeatherResult = httpCode;
            if (httpCode == 200)
            {
                lastWeatherUpdateTime = currentTime;
                updateTOutside = (WeatherService.temperature != getDecimal(getTOutside()));
            }
            else
                TRACE(F("Weather service error: %d\n"), httpCode);
        }
    }

    // Log OpenTherm values from Thermostat and Boiler
    if (currentTime >= otLogTime)
    {
        otLogTime = currentTime + PersistentData.openThermLogInterval;
        logOpenThermValues(false);
        Tracer::traceFreeHeap();
    }

    if ((otLogSyncTime != 0) && (currentTime >= otLogSyncTime))
    {
        if (trySyncOpenThermLog(NULL))
            otLogSyncTime = 0;
        else
        {
            logEvent("Unable to sync OpenTherm Log with FTP server");
            otLogSyncTime += FTP_RETRY_INTERVAL;
        }
    }
}


void initializeOpenThermGateway()
{
    Tracer tracer(F("initializeOpenThermGateway"));

    bool success = setMaxTSet();

    if (success)
        logEvent(F("OTGW initialized"));
}


void resetOpenThermGateway()
{
    Tracer tracer(F("resetOpenThermGateway"));

    OTGW.reset();
    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;

    logEvent(F("OTGW reset"));
}


bool setMaxTSet()
{
    Tracer tracer(F("setMaxTSet"));

    snprintf(stringBuffer, sizeof(stringBuffer), "%d", boilerTSet[BoilerLevel::High]);
    
    bool success = OTGW.sendCommand("SH", stringBuffer); 
    if (!success)
    {
        logEvent(F("Unable to set max CH water setpoint"));
        resetOpenThermGateway();
    }

    return success;
}


bool setTOutside(float temperature)
{
    Tracer tracer(F("setTOutside"));

    snprintf(stringBuffer, sizeof(stringBuffer), "%0.1f", temperature);
    
    bool success = OTGW.sendCommand("OT", stringBuffer); 
    if (!success)
    {
        logEvent(F("Unable to set outside temperature"));
        resetOpenThermGateway();
    }

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    Tracer tracer(F("setBoilerLevel"));

    if (level == currentBoilerLevel)
        return true;

    currentBoilerLevel = level;

    if (level == BoilerLevel::Thermostat)
        totalOverrideDuration += (currentTime - boilerOverrideStartTime);
    else
        boilerOverrideStartTime = currentTime;

    if ((changeBoilerLevelTime != 0) && (level == changeBoilerLevel))
        changeBoilerLevelTime = 0;

    bool success;
    if (level == BoilerLevel::Off)
        success = OTGW.sendCommand("CH", "0");
    else
    {
        sprintf(stringBuffer, "%d", boilerTSet[level]);
        success = OTGW.sendCommand("CS", stringBuffer);
    }

    if (!success)
    {
        logEvent(F("Unable to set boiler level"));
        resetOpenThermGateway();
    }

    return success;
}


float getDecimal(uint16_t dataValue)
{
    if (dataValue == DATA_VALUE_NONE)
        return 0.0;
    else
        return OTGW.getDecimal(dataValue);
}


int8_t getInteger(uint16_t dataValue)
{
    if (dataValue == DATA_VALUE_NONE)
        return 0;
    else
        return static_cast<int8_t>(std::round(OTGW.getDecimal(dataValue)));
}


uint16_t getTOutside()
{
    uint16_t result = boilerResponses[OpenThermDataId::TOutside];
    if ((result == DATA_VALUE_NONE) || (getInteger(result) < -30))
        result = otgwResponses[OpenThermDataId::TOutside];
    return result;
}


void logOpenThermValues(bool forceCreate)
{
    Tracer tracer(F("logOpenThermValues"));

    uint16_t thermostatTSet;
    if (thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable)
        thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    else
        thermostatTSet = 0; // CH disabled

    OpenThermLogEntry* otLogEntryPtr = new OpenThermLogEntry();
    otLogEntryPtr->time = currentTime;
    otLogEntryPtr->thermostatTSet = thermostatTSet;
    otLogEntryPtr->thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
    otLogEntryPtr->boilerStatus = boilerResponses[OpenThermDataId::Status];
    otLogEntryPtr->boilerTSet = boilerResponses[OpenThermDataId::TSet];
    otLogEntryPtr->boilerTWater = boilerResponses[OpenThermDataId::TBoiler];
    otLogEntryPtr->tOutside = getTOutside();
    otLogEntryPtr->repeat = 0;

    if ((lastOTLogEntryPtr != NULL) && (lastOTLogEntryPtr->equals(otLogEntryPtr)) && 
        (lastOTLogEntryPtr->repeat != 255) && !forceCreate)
    {
        lastOTLogEntryPtr->repeat++;
        delete otLogEntryPtr;
    }
    else
    {
        OpenThermLog.add(otLogEntryPtr);
        lastOTLogEntryPtr = otLogEntryPtr;
        if (++otLogEntriesToSync == (OT_LOG_LENGTH / 2))
            otLogSyncTime = currentTime;
    }

    TRACE(F("%d OpenTherm log entries.\n"), OpenThermLog.count());
}


bool trySyncOpenThermLog(Print* printTo)
{
    Tracer tracer(F("trySyncOpenThermLog"));

    if (!FTPClient.begin(FTP_SERVER, FTP_USERNAME, FTP_PASSWORD, FTP_DEFAULT_CONTROL_PORT, printTo))
    {
        FTPClient.end();
        return false;
    }

    bool success = false;
    if (otLogEntriesToSync == 0)
        success = true;
    else
    {
        WiFiClient& dataClient = FTPClient.append("OTGW.csv");
        if (dataClient.connected())
        {
            OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getEntryFromEnd(otLogEntriesToSync));
            writeCsvDataLines(otLogEntryPtr, dataClient);
            dataClient.stop();

            if (FTPClient.readServerResponse() == 226)
            {
                TRACE(F("Successfully appended log entries.\n"));
                otLogEntriesToSync = 0;
                lastOTLogSyncTime = currentTime;
                success = true;
            }
            else
                TRACE(F("FTP Append command failed: %s\n"), FTPClient.getLastResponse());
        }
    }

    FTPClient.end();

    return success;
}


void handleSerialData()
{
    Tracer tracer(F("handleSerialData"));

    OpenThermGatewayMessage otgwMessage = OTGW.readMessage();

    switch (otgwMessage.direction)
    {
        case OpenThermGatewayDirection::FromThermostat:
            handleThermostatRequest(otgwMessage);
            break;

        case OpenThermGatewayDirection::FromBoiler:
            handleBoilerResponse(otgwMessage);
            break;

        case OpenThermGatewayDirection::ToBoiler:
            handleBoilerRequest(otgwMessage);
            break;

        case OpenThermGatewayDirection::ToThermostat:
            handleThermostatResponse(otgwMessage);
            break;

        case OpenThermGatewayDirection::Unexpected:
            if (otgwMessage.message.startsWith("test"))
            {
                test(otgwMessage.message);
                break;
            }

        case OpenThermGatewayDirection::Error:
            snprintf(stringBuffer, sizeof(stringBuffer), "OTGW: %s", otgwMessage.message.c_str());
            logEvent(stringBuffer);
    }
}


void test(String message)
{
    Tracer tracer(F("test"));

    if (message.startsWith("testL"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
            logEvent(F("Test event to fill the event log"));

        for (int i = 0; i < OT_LOG_LENGTH; i++)
        {
            logOpenThermValues(true);
            logOpenThermValues(false);
        }
    }
    else if (message.startsWith("testW"))
    {
        weatherServicePollTime = currentTime;
    }
}


void handleThermostatRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer(F("handleThermostatRequest"));
    
    // Prevent thermostat on/off switching behavior
    if (otFrame.dataId == OpenThermDataId::Status)
    {
        bool masterCHEnable = otFrame.dataValue & OpenThermStatus::MasterCHEnable;
        bool lastMasterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
        if (!masterCHEnable && lastMasterCHEnable)
        {
            // Thermostat switched CH off
            // Keep boiler at Low level for a while.
            if (currentBoilerLevel != BoilerLevel::Low)
                setBoilerLevel(BoilerLevel::Low);
            changeBoilerLevel = BoilerLevel::Thermostat;
            changeBoilerLevelTime = currentTime + KEEP_TSET_LOW_DURATION;
        }
    }
    else if (otFrame.dataId == OpenThermDataId::TSet)
    {
        if (getInteger(otFrame.dataValue) < boilerTSet[BoilerLevel::Low])
        {
            // TSet set below Low level; keep boiler at Low level
            bool lastMasterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
            if (lastMasterCHEnable && (currentBoilerLevel != BoilerLevel::Low))
            {
                setBoilerLevel(BoilerLevel::Low);
            }
        }
        else
        {
            // TSet set above Low level
            if (thermostatRequests[OpenThermDataId::MaxRelModulation] == 0)
            {
                // Thermostat is in "Low Load Mode" (using on/off switching)
                // Keep boiler at Low level for a while before letting thermostat control TSet again.
                if (currentBoilerLevel != BoilerLevel::Low)
                {
                    setBoilerLevel(BoilerLevel::Low);
                    changeBoilerLevel = BoilerLevel::Thermostat;
                    changeBoilerLevelTime = currentTime + KEEP_TSET_LOW_DURATION;
                }
            }
            else
            {
                // Thermostat is requesting more than minimal Modulation (not in "Low Load Mode").
                // Let thermostat control boiler TSet again.
                if (currentBoilerLevel != BoilerLevel::Thermostat)
                    setBoilerLevel(BoilerLevel::Thermostat);
            }
        }
    }

    thermostatRequests[otFrame.dataId] = otFrame.dataValue;
}


void handleBoilerResponse(OpenThermGatewayMessage otFrame)
{
    Tracer tracer(F("handleBoilerResponse"));

    if (otFrame.msgType == OpenThermMsgType::UnknownDataId)
        return;
    
    boilerResponses[otFrame.dataId] = otFrame.dataValue;
}


void handleBoilerRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer(F("handleBoilerRequest"));

    // Modified request from OTGW to boiler (e.g. TSet override)
    otgwRequests[otFrame.dataId] = otFrame.dataValue;
}


void handleThermostatResponse(OpenThermGatewayMessage otFrame)
{
    Tracer tracer(F("handleThermostatResponse"));

    // Modified response from OTGW to thermostat (e.g. TOutside override)
    otgwResponses[otFrame.dataId] = otFrame.dataValue;

    if (otFrame.dataId == OpenThermDataId::MaxTSet)
    {
        int8_t maxTSet = getInteger(otFrame.dataValue);
        if (maxTSet != boilerTSet[BoilerLevel::High])
        {
            logEvent(F("Max CH Water Setpoint is changed (by OTGW reset?)"));
            otgwInitializeTime = currentTime;
        }
    }
}


void writeHtmlHeader(String title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.hostName, title.c_str());
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"196x196\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"196x196\" href=\"%s\">\r\n"), ICON, ICON);
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , HTTP_POLL_INTERVAL);
    HttpResponse.println(F("</head>"));
    
    HttpResponse.println(F("<body>"));
    if (includeHomePageLink)
        HttpResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
    if (includeHeading)
        HttpResponse.printf(F("<h1>%s</h1>\r\n"), title.c_str());
}


void writeHtmlFooter()
{
    HttpResponse.println(F("</body>"));
    HttpResponse.println(F("</html>"));
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    uint16_t burnerHours = boilerResponses[OpenThermDataId::BoilerBurnerHours];
    float startsPerHour;
    if ((burnerHours == DATA_VALUE_NONE) || (burnerHours == 0))
        startsPerHour = 0.0;
    else
        startsPerHour = float(boilerResponses[OpenThermDataId::BoilerBurnerStarts]) / burnerHours; 

    writeHtmlHeader(F("Home"), false, false);

    HttpResponse.println(F("<h1>Last OpenTherm values</h1>"));

    HttpResponse.println(F("<h2>Thermostat</h2>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), OTGW.getMasterStatus(thermostatRequests[OpenThermDataId::Status]));
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>Max Modulation %%</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation]));
    HttpResponse.printf(F("<tr><td>Max TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxTSet]));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h2>Boiler</h2>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), OTGW.getSlaveStatus(boilerResponses[OpenThermDataId::Status]));
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>TWater</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TBoiler]));
    HttpResponse.printf(F("<tr><td>TOutside</td><td>%0.1f</td></tr>\r\n"), getDecimal(getTOutside()));
    HttpResponse.printf(F("<tr><td>Fault flags</td><td>%s</td></tr>\r\n"), OTGW.getFaultFlags(boilerResponses[OpenThermDataId::SlaveFault]));
    HttpResponse.printf(F("<tr><td>Burner starts</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerStarts]);
    HttpResponse.printf(F("<tr><td>CH burner on</td><td>%d h</td></tr>\r\n"), burnerHours);
    HttpResponse.printf(F("<tr><td>DHW burner on</td><td>%d h</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerDHWBurnerHours]);
    HttpResponse.printf(F("<tr><td>Starts/hour</td><td>%0.1f</td></tr>\r\n"), startsPerHour);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p class=\"traffic\"><a href=\"/traffic\">View all OpenTherm traffic</a></p>"));

    HttpResponse.println(F("<h1>Boiler override</h1>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Current level</td><td>%s</td></tr>\r\n"), BOILER_LEVEL_NAMES[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        HttpResponse.printf(F("<tr><td>Change to</td><td>%s</td></tr>\r\n"), BOILER_LEVEL_NAMES[changeBoilerLevel]);
        HttpResponse.printf(F("<tr><td>Change in</td><td>%d s</td></tr>\r\n"), changeBoilerLevelTime - currentTime);
    }
    HttpResponse.printf(F("<tr><td>Override duration</td><td>%0.1f h</td></tr>\r\n"), float(totalOverrideDuration) / 3600);
    HttpResponse.printf(F("<tr><td>Weather result</td><td>%d</td></tr>\r\n"), lastWeatherResult);
    if (lastWeatherUpdateTime != 0)
    {
        formatTime(stringBuffer, sizeof(stringBuffer), "%H:%M", lastWeatherUpdateTime);
        HttpResponse.printf(F("<tr><td>Weather update</td><td>%s</td></tr>\r\n"), stringBuffer);
    }
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>OpenTherm Gateway status</h1>"));
    HttpResponse.println(F("<table>"));
    for (int i = 0; i <= 4; i++)
        HttpResponse.printf(F("<tr><td>Error %02X</td><td>%u</td></tr>\r\n"), i, OTGW.errors[i]);
    HttpResponse.printf(F("<tr><td>OTGW Resets</td><td>%u</td></tr>\r\n"), OTGW.resets);
    HttpResponse.printf(F("<tr><td>ESP Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>ESP Uptime</td><td>%0.1f days</td></tr>\r\n"), float(currentTime - initTime) / 86400);
    if (lastOTLogSyncTime != 0)
    {
        formatTime(stringBuffer, sizeof(stringBuffer), "%H:%M", lastOTLogSyncTime);
        HttpResponse.printf(F("<tr><td>FTP Sync</td><td>%s</td></tr>\r\n"), stringBuffer);
    }
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<p class=\"log\"><a href=\"/log\">%d OpenTherm log entries.</a></p>\r\n"), OpenThermLog.count());
    HttpResponse.println(F("<p class=\"cmd\"><a href=\"/cmd\">Send command to OpenTherm Gateway</a></p>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void writeHtmlOpenThermDataTable(String title, uint16_t* otDataTable)
{
    HttpResponse.printf(F("<h2>%s</h2>\r\n"), title.c_str());
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Data ID</th><th>Data Value (hex)</th><th>Data value (dec)</th></tr>"));

    for (int i = 0; i < 256; i++)
    {
      uint16_t dataValue = otDataTable[i];
      if (dataValue == DATA_VALUE_NONE)
        continue;
        HttpResponse.printf
            (F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), 
            i, 
            dataValue, 
            getDecimal(dataValue)
            );
    }

    HttpResponse.println(F("</table>"));
}


void handleHttpOpenThermTrafficRequest()
{
    Tracer tracer(F("handleHttpOpenThermTrafficRequest"));

    writeHtmlHeader(F("OpenTherm traffic"), true, true);
    
    writeHtmlOpenThermDataTable(F("Thermostat requests"), thermostatRequests);
    writeHtmlOpenThermDataTable(F("Boiler responses"), boilerResponses);
    writeHtmlOpenThermDataTable(F("OTGW requests (thermostat overrides)"), otgwRequests);
    writeHtmlOpenThermDataTable(F("OTGW responses (boiler overrides)"), otgwResponses);

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    writeHtmlHeader(F("OpenTherm log"), true, true);
    
    HttpResponse.println(F("<p class=\"log-csv\"><a href=\"/log-csv\">Get log in CSV format</a></p>"));
    HttpResponse.printf(
        F("<p class=\"log-sync\"><a href=\"/log/sync\">Sync %d log entries with FTP Server</a></p>"),
        otLogEntriesToSync
        );

    // If the OT log contains many entries, we render every other so the max HTTP response size is not exceeded.
    bool skipEvenEntries = OpenThermLog.count() > (OT_LOG_LENGTH / 2);

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Time</th><th>TSet(t)</th><th>Max mod %</th><th>TSet(b)</th><th>TWater</th><th>TOutside</th><th>Status</th></tr>"));
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(stringBuffer, sizeof(stringBuffer), "%H:%M", otLogEntryPtr->time);

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%s</td></tr>\r\n"),
            stringBuffer, 
            getDecimal(otLogEntryPtr->thermostatTSet),
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation),
            getDecimal(otLogEntryPtr->boilerTSet),
            getDecimal(otLogEntryPtr->boilerTWater),
            getDecimal(otLogEntryPtr->tOutside),
            OTGW.getSlaveStatus(otLogEntryPtr->boilerStatus)
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getNextEntry());
        if (skipEvenEntries && (otLogEntryPtr != NULL))
            otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getNextEntry());
    }
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpOpenThermLogSyncRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogSyncRequest"));

    writeHtmlHeader("FTP Sync", true, true);

    HttpResponse.printf(
        F("<p>Sending %d OpenTherm log entries to FTP server (%s) ...</p>\r\n"), 
        otLogEntriesToSync,
        FTP_SERVER
        );

    HttpResponse.println("<div><pre>");
    bool success = trySyncOpenThermLog(&HttpResponse); 
    HttpResponse.println("</pre></div>");

    if (success)
    {
        HttpResponse.println("<p>Success!</p>");
        otLogSyncTime = 0; // Cancel scheduled sync (if any)
    }
    else
        HttpResponse.println("<p>Failed!</p>");
 
    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpOpenThermLogCsvRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogCsvRequest"));

    /* CSV format:
    "Time","TSet(t)","Max Modulation %","TSet(b)","TWater","TOutside","CH","DHW"
    "2019-02-02 12:30",45,100,45,44,15,5,0
    "2019-02-02 12:35",50,100,48,65,14,0,5
    */

    HttpResponse.clear();
    HttpResponse.println(F("\"Time\",\"TSet(t)\",\"Max Modulation %\",\"TSet(b)\",\"TWater\",\"TOutside\",\"CH\",\"DHW\""));

    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getFirstEntry());
    writeCsvDataLines(otLogEntryPtr, HttpResponse);

    WebServer.send(200, F("text/plain"), HttpResponse);
}


void writeCsvDataLines(OpenThermLogEntry* otLogEntryPtr, Print& destination)
{
    while (otLogEntryPtr != NULL)
    {
        time_t otLogEntryTime = otLogEntryPtr->time;
        writeCsvDataLine(otLogEntryPtr, otLogEntryTime, destination);
        if (otLogEntryPtr->repeat > 0)
        {
            // OpenTherm log entry repeats at least once.
            // Write an additional CSV data line for the end of the interval to prevent interpolation in the graphs. 
            otLogEntryTime += (PersistentData.openThermLogInterval * otLogEntryPtr->repeat);
            writeCsvDataLine(otLogEntryPtr, otLogEntryTime, destination);
        }
        
        otLogEntryPtr = static_cast<OpenThermLogEntry*>(OpenThermLog.getNextEntry());
    }
}


void writeCsvDataLine(OpenThermLogEntry* otLogEntryPtr, time_t time, Print& destination)
{
    int statusCH = 0;
    int statusDHW = 0;
    if (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveFlame)
    {
        statusCH = (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveCHMode) ? 5 : 0;
        statusDHW = (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveDHWMode) ? 5 : 0;
    }

    formatTime(stringBuffer, sizeof(stringBuffer), "%F %H:%M", time);

    destination.printf(
        "\"%s\",%d,%d,%d,%d,%d,%d,%d\r\n", 
        stringBuffer, 
        getInteger(otLogEntryPtr->thermostatTSet),
        getInteger(otLogEntryPtr->thermostatMaxRelModulation),
        getInteger(otLogEntryPtr->boilerTSet),
        getInteger(otLogEntryPtr->boilerTWater),
        getInteger(otLogEntryPtr->tOutside),
        statusCH,
        statusDHW
        );
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    writeHtmlHeader(F("Event log"), true, true);

    char* event = static_cast<char*>(EventLog.getFirstEntry());
    while (event != NULL)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = static_cast<char*>(EventLog.getNextEntry());
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpEventLogClearRequest()
{
    Tracer tracer(F("handleHttpEventLogClearRequest"));

    EventLog.clear();
    logEvent(F("Event log cleared."));

    handleHttpEventLogRequest();
}


void addTextBoxRow(StringBuilder& output, String name, String value, String label)
{
    output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\"></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        value.c_str()
        );
}


void handleHttpCommandFormRequest()
{
    Tracer tracer(F("handleHttpCommandFormRequest"));

    writeHtmlHeader(F("Send OTGW Command"), true, true);

    HttpResponse.println(F("<form action=\"/cmd\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("cmd"), F("PR"), F("Command"));
    addTextBoxRow(HttpResponse, F("value"), F("A"), F("Value"));
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.println(F("<h2>OTGW Response</h2>"));
    HttpResponse.printf(F("<div class=\"response\"><pre>%s</pre></div>"), stringBuffer);

    writeHtmlFooter();

    stringBuffer[0] = 0;

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpCommandFormPost()
{
    Tracer tracer(("handleHttpCommandFormPost"));

    String cmd = WebServer.arg("cmd");
    String value = WebServer.arg("value");

    TRACE(F("cmd: '%s'\nvalue: '%s'\n"), cmd.c_str(), value.c_str());

    if (cmd.length() != 2)
        snprintf(stringBuffer, sizeof(stringBuffer), "Invalid command. Must be 2 characters.");
    else
    {
        bool success = OTGW.sendCommand(cmd.c_str(), value.c_str(), stringBuffer, sizeof(stringBuffer));
        if (!success)
            snprintf(stringBuffer, sizeof(stringBuffer), "No valid response received from OTGW.");
    }

    handleHttpCommandFormRequest();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    char tzOffsetString[4];
    sprintf(tzOffsetString, "%d", PersistentData.timeZoneOffset);

    char otLogIntervalString[4];
    sprintf(otLogIntervalString, "%u", PersistentData.openThermLogInterval);

    writeHtmlHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("hostName"), PersistentData.hostName, F("Host name"));
    addTextBoxRow(HttpResponse, F("tzOffset"), tzOffsetString, F("Timezone offset"));
    addTextBoxRow(HttpResponse, F("otLogInterval"), otLogIntervalString, F("OT Log Interval"));
    addTextBoxRow(HttpResponse, F("weatherApiKey"), PersistentData.weatherApiKey, F("Weather API Key"));
    addTextBoxRow(HttpResponse, F("weatherLocation"), PersistentData.weatherLocation, F("Weather Location"));
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.printf(
        F("<div>OpenTherm log length: minimal %d * %d s = %0.1f hours</div>"), 
        OT_LOG_LENGTH, 
        PersistentData.openThermLogInterval,
        float(OT_LOG_LENGTH * PersistentData.openThermLogInterval) / 3600
        );

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    String tzOffsetString = WebServer.arg("tzOffset");
    String otLogIntervalString = WebServer.arg("otLogInterval");

    strcpy(PersistentData.hostName, WebServer.arg("hostName").c_str()); 
    strcpy(PersistentData.weatherApiKey, WebServer.arg("weatherApiKey").c_str()); 
    strcpy(PersistentData.weatherLocation, WebServer.arg("weatherLocation").c_str()); 

    PersistentData.timeZoneOffset = static_cast<int8_t>(atoi(tzOffsetString.c_str()));
    PersistentData.openThermLogInterval = static_cast<uint16_t>(atoi(otLogIntervalString.c_str()));

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset; 

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
