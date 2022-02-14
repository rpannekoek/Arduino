#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <OTGW.h>
#include <Log.h>
#include <WeatherAPI.h>
#include "PersistentData.h"
#include "OpenThermLogEntry.h"
#include "GlobalLogEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"
#define SECONDS_PER_DAY (24 * 3600)
#define WATCHDOG_INTERVAL_MS 1000
#define OTGW_STARTUP_TIME 5
#define OTGW_TIMEOUT 60
#define HTTP_POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define EVENT_LOG_LENGTH 50
#define OT_LOG_LENGTH 240
#define KEEP_TSET_LOW_DURATION (11 * 60)
#define WEATHER_SERVICE_POLL_INTERVAL (15 * 60)
#define WEATHER_SERVICE_RESPONSE_TIMEOUT 10
#define FTP_RETRY_INTERVAL (60 * 60)
#define GLOBAL_LOG_INTERVAL (30 * 60)

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_TZ_OFFSET F("tzOffset")
#define CFG_OT_LOG_INTERVAL F("otLogInterval")
#define CFG_FTP_SYNC_ENTRIES F("ftpSyncEntries")
#define CFG_WEATHER_API_KEY F("weatherApiKey")
#define CFG_WEATHER_LOC F("weatherLocation")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";

const char* BoilerLevelNames[5] = {"Off", "Low", "Medium", "High", "Thermostat"};

enum BoilerLevel // Unscoped enum so it can be used as array index without casting
{
    Off,
    Low,
    Medium,
    High,
    Thermostat
};

const int boilerTSet[5] = {0, 40, 50, 60, 0};


OpenThermGateway OTGW(Serial, 14);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(SECONDS_PER_DAY); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
WeatherAPI WeatherService(2000); // 2 sec request timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
Log<OpenThermLogEntry> OpenThermLog(OT_LOG_LENGTH);
Log<GlobalLogEntry> GlobalLog(24 * 2);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

// OpenTherm data values indexed by data ID
uint16_t thermostatRequests[256];
uint16_t boilerResponses[256];
uint16_t otgwRequests[256];
uint16_t otgwResponses[256];

uint32_t watchdogFeedTime = 0;
time_t currentTime = 0;
time_t otLogTime = 0;
time_t globalLogTime = 0;
time_t otgwInitializeTime = OTGW_STARTUP_TIME;
time_t otgwTimeout = OTGW_TIMEOUT;
time_t weatherServicePollTime = 0;
time_t weatherServiceTimeout = 0;
time_t lastWeatherUpdateTime = 0;
time_t actionPerformedTime = 0;

bool updateTOutside = false;

OpenThermLogEntry* lastOTLogEntryPtr = nullptr;
GlobalLogEntry* lastGlobalLogEntryPtr = nullptr;

uint16_t otLogEntriesToSync = 0;
time_t otLogSyncTime = 0;
time_t lastOTLogSyncTime = 0;

BoilerLevel currentBoilerLevel = BoilerLevel::Thermostat;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime = 0;
time_t boilerOverrideStartTime = 0;
uint32_t totalOverrideDuration = 0;

char stringBuffer[128];
String otgwResponse;

const char* formatTime(const char* format, time_t time)
{
    static char timeString[32];
    strftime(timeString, sizeof(timeString), format, gmtime(&time));
    return timeString;
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
    digitalWrite(LED_BUILTIN, LED_ON);

    Serial.begin(9600);
    Serial.setTimeout(1000);
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    Html.setTitlePrefix(PersistentData.hostName);

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/ot", handleHttpOpenThermRequest);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log", handleHttpOpenThermLogRequest);
    WebServer.on("/log/sync", handleHttpOpenThermLogSyncRequest);
    WebServer.on("/log-csv", handleHttpOpenThermLogCsvRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/cmd", HTTP_GET, handleHttpCommandFormRequest);
    WebServer.on("/cmd", HTTP_POST, handleHttpCommandFormPost);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    memset(stringBuffer, 0, sizeof(stringBuffer));

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));
    memset(otgwRequests, 0xFF, sizeof(otgwRequests));
    memset(otgwResponses, 0xFF, sizeof(otgwResponses));

    WiFiSM.on(WiFiInitState::TimeServerInitializing, onTimeServerInit);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();

    if (millis() >= watchdogFeedTime)
    {
        OTGW.feedWatchdog();
        watchdogFeedTime = millis() + WATCHDOG_INTERVAL_MS;
    }

    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, LED_ON);
        handleSerialData();
        otgwTimeout = currentTime + OTGW_TIMEOUT;
        digitalWrite(LED_BUILTIN, LED_OFF);
        return;
    }

    if (currentTime >= otgwTimeout)
    {
        TRACE(F("currentTime=%u; otgwTimeout=%u\n"), currentTime, otgwTimeout);
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
    // After time server sync all times leap ahead from 1-1-1970
    otgwTimeout = currentTime + OTGW_TIMEOUT;
    if (otgwInitializeTime != 0) 
        otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    if (changeBoilerLevelTime != 0) 
        changeBoilerLevelTime = currentTime;
    if (boilerOverrideStartTime != 0)
        boilerOverrideStartTime = currentTime;

    // Create first global log entry
    lastGlobalLogEntryPtr = new GlobalLogEntry();
    lastGlobalLogEntryPtr->time = currentTime - (currentTime % GLOBAL_LOG_INTERVAL);
    GlobalLog.add(lastGlobalLogEntryPtr);
    globalLogTime = currentTime;
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
            weatherServiceTimeout = 0;
            return;
        }

        int httpCode = WeatherService.endRequestData();
        if (httpCode != 0)
        {
            weatherServiceTimeout = 0;
            if (httpCode == 200)
            {
                lastWeatherUpdateTime = currentTime;
                updateTOutside = (WeatherService.temperature != getDecimal(getTOutside()));
            }
            else
            {
                String event = F("Weather service error: ");
                event += httpCode;
                logEvent(event);
            }
        }
    }

    if (currentTime >= globalLogTime)
    {
        globalLogTime++;
        updateGlobalLog();
    }

    // Log OpenTherm values from Thermostat and Boiler
    if (currentTime >= otLogTime)
    {
        otLogTime = currentTime + PersistentData.openThermLogInterval;
        logOpenThermValues(false);
    }

    if ((otLogSyncTime != 0) && (currentTime >= otLogSyncTime))
    {
        if (trySyncOpenThermLog(nullptr))
        {
            logEvent(F("FTP synchronized"));
            otLogSyncTime = 0;
        }
        else
        {
            String event = F("FTP sync failed: ");
            event += FTPClient.getLastResponse();
            logEvent(event);
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
        return static_cast<int8_t>(round(OTGW.getDecimal(dataValue)));
}


uint16_t getTOutside()
{
    uint16_t result = boilerResponses[OpenThermDataId::TOutside];
    if ((result == DATA_VALUE_NONE) || (getInteger(result) < -30))
        result = otgwResponses[OpenThermDataId::TOutside];
    return result;
}


uint16_t getTReturn()
{
    uint16_t result = boilerResponses[OpenThermDataId::TReturn];
    if (result == DATA_VALUE_NONE)
        result = otgwResponses[OpenThermDataId::TReturn];
    return result;
}


void updateGlobalLog()
{
    float tWater = getDecimal(boilerResponses[OpenThermDataId::TBoiler]);
    float tReturn = getDecimal(getTReturn());
    bool flame = (boilerResponses[OpenThermDataId::Status] & OpenThermStatus::SlaveFlame) != 0;

    if (currentTime >= lastGlobalLogEntryPtr->time + GLOBAL_LOG_INTERVAL)
    {
        lastGlobalLogEntryPtr = new GlobalLogEntry();
        lastGlobalLogEntryPtr->time = currentTime;
        GlobalLog.add(lastGlobalLogEntryPtr);
    }
    lastGlobalLogEntryPtr->update(tWater, tReturn, flame);
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
    otLogEntryPtr->tReturn = getTReturn();
    otLogEntryPtr->tOutside = getTOutside();
    otLogEntryPtr->repeat = 0;

    if ((lastOTLogEntryPtr != nullptr) && (lastOTLogEntryPtr->equals(otLogEntryPtr)) && 
        (lastOTLogEntryPtr->repeat != 255) && !forceCreate)
    {
        lastOTLogEntryPtr->repeat++;
        delete otLogEntryPtr;
    }
    else
    {
        OpenThermLog.add(otLogEntryPtr);
        lastOTLogEntryPtr = otLogEntryPtr;
        if (++otLogEntriesToSync == PersistentData.ftpSyncEntries)
            otLogSyncTime = currentTime;
        if (otLogEntriesToSync > OT_LOG_LENGTH)
            otLogEntriesToSync = OT_LOG_LENGTH;
    }

    TRACE(F("%d OpenTherm log entries.\n"), OpenThermLog.count());
}


bool trySyncOpenThermLog(Print* printTo)
{
    Tracer tracer(F("trySyncOpenThermLog"));

    if (!FTPClient.begin(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo))
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
            OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync);
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
        {
            logEvent(F("Test event to fill the event log"));
            yield();
            OTGW.feedWatchdog();
        }

        for (int i = 0; i < OT_LOG_LENGTH; i++)
        {
            logOpenThermValues(true);
            logOpenThermValues(false);
            yield();
            OTGW.feedWatchdog();
        }
    }
    else if (message.startsWith("testW"))
    {
        weatherServicePollTime = currentTime;
    }
    else if (message.startsWith("testH"))
    {
        int tWater = 40;
        for (int i = 0; i < GLOBAL_LOG_INTERVAL; i++)
        {
            lastGlobalLogEntryPtr->update(tWater, tWater - 10, (i % 2 == 0));
            if (tWater++ == 60) tWater = 40;
        }
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


bool shouldPerformAction(String name)
{
    if (!WebServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = WebServer.arg(name).toInt();

    if (actionTime == actionPerformedTime)
        return false; // Action already performed

    actionPerformedTime = actionTime;
    return true;
}


float getBarValue(float t, float tMin = 20)
{
    return float(t - tMin) / (boilerTSet[BoilerLevel::High] - tMin);
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    uint32_t otgwErrors = 0;
    for (int i = 0; i <= 4; i++)
        otgwErrors += OTGW.errors[i];

    float thermostatTSet = getDecimal(thermostatRequests[OpenThermDataId::TSet]);
    float boilerTSet = getDecimal(boilerResponses[OpenThermDataId::TSet]);
    float boilerTWater = getDecimal(boilerResponses[OpenThermDataId::TBoiler]);
    float tReturn = getDecimal(getTReturn());
    float tOutside = getDecimal(getTOutside());

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>OpenTherm Gateway status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>OTGW Errors</th><td>%u</td></tr>\r\n"), otgwErrors);
    HttpResponse.printf(F("<tr><th>OTGW Resets</th><td>%u</td></tr>\r\n"), OTGW.resets);
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    if (PersistentData.ftpSyncEntries == 0)
        HttpResponse.println(F("<tr><th>FTP Sync</th><td>Disabled</td></tr>"));
    else
        HttpResponse.printf(F("<tr><th>FTP Sync</th><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastOTLogSyncTime));
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/log\">OpenTherm log</a></th><td>%d</td></tr>\r\n"), OpenThermLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1><a href=\"/ot\">Current OpenTherm values</a></h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(
        F("<tr><th>Thermostat</th><td>%0.1f °C @ %0.0f %%</td><td class=\"graph\">"),
        thermostatTSet,
        getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation])       
        );
    Html.writeBar(getBarValue(thermostatTSet), F("setBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>set</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        boilerTSet
        );
    Html.writeBar(getBarValue(boilerTSet), F("setBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>water</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        boilerTWater
        );
    Html.writeBar(getBarValue(boilerTWater), F("waterBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>return</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        tReturn
        );
    Html.writeBar(getBarValue(tReturn), F("returnBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>outside</sub></th><td>%0.1f °C</td><td class=\"graph\"></td></tr>"),
        tOutside
        );
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Last 24 hours</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th rowspan='2'>Flame</th><th colspan='3'>T<sub>water</sub> (°C)</th><th colspan='3'>T<sub>return</sub> (°C)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));
    GlobalLogEntry* logEntryPtr = GlobalLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)GLOBAL_LOG_INTERVAL);
        float avgTWater = logEntryPtr->sumTWater / seconds;
        float avgTReturn = logEntryPtr->sumTReturn / seconds;
        float avgDeltaT = avgTWater - avgTReturn;

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%d %%</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            100 * logEntryPtr->flameCount / seconds,
            logEntryPtr->minTWater,
            logEntryPtr->maxTWater,
            avgTWater,
            logEntryPtr->minTReturn,
            logEntryPtr->maxTReturn,
            avgTReturn
            );

        Html.writeStackedBar(getBarValue(avgTReturn), getBarValue(avgDeltaT), F("returnBar"), F("waterBar"), false, false);

        HttpResponse.println(F("</td></tr>"));
        logEntryPtr = GlobalLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpOpenThermRequest()
{
    Tracer tracer(F("handleHttpOpenThermRequest"));

    uint16_t burnerStarts = boilerResponses[OpenThermDataId::BoilerBurnerStarts];
    float avgBurnerOnTime;
    if ((burnerStarts == DATA_VALUE_NONE) || (burnerStarts == 0))
        avgBurnerOnTime = 0.0;
    else
        avgBurnerOnTime =  float(boilerResponses[OpenThermDataId::BoilerBurnerHours] * 60)  
            / boilerResponses[OpenThermDataId::BoilerBurnerStarts]; 

    Html.writeHeader(F("Current OpenTherm values"), true, true);

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
    HttpResponse.printf(F("<tr><td>Burner starts</td><td>%d</td></tr>\r\n"), burnerStarts);
    HttpResponse.printf(F("<tr><td>Burner on</td><td>%d h</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    HttpResponse.printf(F("<tr><td>Burner on DHW</td><td>%d h</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerDHWBurnerHours]);
    HttpResponse.printf(F("<tr><td>Avg burner on</td><td>%0.1f min</td></tr>\r\n"), avgBurnerOnTime);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Boiler override</h1>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Current level</td><td>%s</td></tr>\r\n"), BoilerLevelNames[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        HttpResponse.printf(F("<tr><td>Change to</td><td>%s</td></tr>\r\n"), BoilerLevelNames[changeBoilerLevel]);
        HttpResponse.printf(F("<tr><td>Change in</td><td>%d s</td></tr>\r\n"), changeBoilerLevelTime - currentTime);
    }
    HttpResponse.printf(F("<tr><td>Override duration</td><td>%0.1f h</td></tr>\r\n"), float(totalOverrideDuration) / 3600);
    if (lastWeatherUpdateTime != 0)
        HttpResponse.printf(F("<tr><td>Weather update</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastWeatherUpdateTime));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p class=\"traffic\"><a href=\"/traffic\">View all OpenTherm traffic</a></p>"));
    HttpResponse.println(F("<p class=\"cmd\"><a href=\"/cmd\">Send command to OpenTherm Gateway</a></p>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
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

    Html.writeHeader(F("OpenTherm traffic"), true, true);
    
    writeHtmlOpenThermDataTable(F("Thermostat requests"), thermostatRequests);
    writeHtmlOpenThermDataTable(F("Boiler responses"), boilerResponses);
    writeHtmlOpenThermDataTable(F("OTGW requests (thermostat overrides)"), otgwRequests);
    writeHtmlOpenThermDataTable(F("OTGW responses (boiler overrides)"), otgwResponses);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    Html.writeHeader(F("OpenTherm log"), true, true);
    
    HttpResponse.println(F("<p class=\"log-csv\"><a href=\"/log-csv\">Get log in CSV format</a></p>"));
    HttpResponse.printf(
        F("<p class=\"log-sync\"><a href=\"/log/sync\">Sync %d log entries with FTP Server</a></p>"),
        otLogEntriesToSync
        );

    // If the OT log contains many entries, we render every other so the max HTTP response size is not exceeded.
    bool skipEvenEntries = OpenThermLog.count() > (OT_LOG_LENGTH / 2);

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Time</th><th>TSet(t)</th><th>Max mod %</th><th>TSet(b)</th><th>TWater</th><th>TReturn</th><th>TOutside</th><th>Status</th></tr>"));
    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    while (otLogEntryPtr != nullptr)
    {
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%s</td></tr>\r\n"),
            formatTime("%H:%M", otLogEntryPtr->time), 
            getDecimal(otLogEntryPtr->thermostatTSet),
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation),
            getDecimal(otLogEntryPtr->boilerTSet),
            getDecimal(otLogEntryPtr->boilerTWater),
            getDecimal(otLogEntryPtr->tReturn),
            getDecimal(otLogEntryPtr->tOutside),
            OTGW.getSlaveStatus(otLogEntryPtr->boilerStatus)
            );

        otLogEntryPtr = OpenThermLog.getNextEntry();
        if (skipEvenEntries && (otLogEntryPtr != nullptr))
            otLogEntryPtr = OpenThermLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpOpenThermLogSyncRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogSyncRequest"));

    Html.writeHeader(F("FTP Sync"), true, true);

    HttpResponse.printf(
        F("<p>Sending %d OpenTherm log entries to FTP server (%s) ...</p>\r\n"), 
        otLogEntriesToSync,
        PersistentData.ftpServer
        );

    HttpResponse.println(F("<div><pre>"));
    bool success = trySyncOpenThermLog(&HttpResponse); 
    HttpResponse.println(F("</pre></div>"));

    if (success)
    {
        HttpResponse.println(F("<p>Success!</p>"));
        otLogSyncTime = 0; // Cancel scheduled sync (if any)
    }
    else
        HttpResponse.println(F("<p>Failed!</p>"));
 
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
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
    HttpResponse.println(F("\"Time\",\"TSet(t)\",\"Max Modulation %\",\"TSet(b)\",\"TWater\",\"TReturn\",\"TOutside\",\"CH\",\"DHW\""));

    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    writeCsvDataLines(otLogEntryPtr, HttpResponse);

    WebServer.send(200, ContentTypeText, HttpResponse);
}


void writeCsvDataLines(OpenThermLogEntry* otLogEntryPtr, Print& destination)
{
    while (otLogEntryPtr != nullptr)
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
        
        otLogEntryPtr = OpenThermLog.getNextEntry();
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

    destination.printf(
        "\"%s\",%d,%d,%d,%d,%d,%d,%d,%d\r\n", 
        formatTime("%F %H:%M", time), 
        getInteger(otLogEntryPtr->thermostatTSet),
        getInteger(otLogEntryPtr->thermostatMaxRelModulation),
        getInteger(otLogEntryPtr->boilerTSet),
        getInteger(otLogEntryPtr->boilerTWater),
        getInteger(otLogEntryPtr->tReturn),
        getInteger(otLogEntryPtr->tOutside),
        statusCH,
        statusDHW
        );
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), true, true);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear event log</a></p>\r\n"), currentTime);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCommandFormRequest()
{
    Tracer tracer(F("handleHttpCommandFormRequest"));

    Html.writeHeader(F("Send OTGW Command"), true, true);

    HttpResponse.println(F("<form action=\"/cmd\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    Html.writeTextBox(F("cmd"), F("Command"), F("PR"), 2);
    Html.writeTextBox(F("value"), F("Value"), F("A"), 16);
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.println(F("<h2>OTGW Response</h2>"));
    HttpResponse.printf(F("<div class=\"response\"><pre>%s</pre></div>"), otgwResponse.c_str());

    Html.writeFooter();

    otgwResponse = String();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCommandFormPost()
{
    Tracer tracer(("handleHttpCommandFormPost"));

    String cmd = WebServer.arg("cmd");
    String value = WebServer.arg("value");

    TRACE(F("cmd: '%s' value: '%s'\n"), cmd.c_str(), value.c_str());

    if (cmd.length() != 2)
        otgwResponse = F("Invalid command. Must be 2 characters.");
    else
    {
        bool success = OTGW.sendCommand(cmd.c_str(), value.c_str(), stringBuffer, sizeof(stringBuffer));
        if (success)
            otgwResponse = stringBuffer;
        else
            otgwResponse = F("No valid response received from OTGW.");
    }

    handleHttpCommandFormRequest();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1);
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1);
    Html.writeTextBox(CFG_TZ_OFFSET, F("Timezone offset"), String(PersistentData.timeZoneOffset), 3);
    Html.writeTextBox(CFG_OT_LOG_INTERVAL, F("OpenTherm Log Interval (s)"), String(PersistentData.openThermLogInterval), 4);
    Html.writeTextBox(CFG_FTP_SYNC_ENTRIES, F("FTP Sync Entries"), String(PersistentData.ftpSyncEntries), 4);
    Html.writeTextBox(CFG_WEATHER_API_KEY, F("Weather API Key"), PersistentData.weatherApiKey, 16);
    Html.writeTextBox(CFG_WEATHER_LOC, F("Weather Location"), PersistentData.weatherLocation, 16);
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.printf(
        F("<div>OpenTherm log length: minimal %d * %d s = %0.1f hours</div>"), 
        OT_LOG_LENGTH, 
        PersistentData.openThermLogInterval,
        float(OT_LOG_LENGTH * PersistentData.openThermLogInterval) / 3600
        );

    if (PersistentData.ftpSyncEntries == 0)
        HttpResponse.println(F("<div>FTP Sync disabled.</div>"));    
    else
        HttpResponse.printf(
            F("<div>FTP Sync interval: minimal %d * %d s = %0.1f hours</div>"), 
            PersistentData.ftpSyncEntries, 
            PersistentData.openThermLogInterval,
            float(PersistentData.ftpSyncEntries * PersistentData.openThermLogInterval) / 3600
            );

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void copyString(const String& input, char* buffer, size_t bufferSize)
{
    strncpy(buffer, input.c_str(), bufferSize);
    buffer[bufferSize - 1] = 0;
}

void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    copyString(WebServer.arg(CFG_WIFI_SSID), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID)); 
    copyString(WebServer.arg(CFG_WIFI_KEY), PersistentData.wifiKey, sizeof(PersistentData.wifiKey)); 
    copyString(WebServer.arg(CFG_HOST_NAME), PersistentData.hostName, sizeof(PersistentData.hostName)); 
    copyString(WebServer.arg(CFG_NTP_SERVER), PersistentData.ntpServer, sizeof(PersistentData.ntpServer)); 
    copyString(WebServer.arg(CFG_FTP_SERVER), PersistentData.ftpServer, sizeof(PersistentData.ftpServer)); 
    copyString(WebServer.arg(CFG_FTP_USER), PersistentData.ftpUser, sizeof(PersistentData.ftpUser)); 
    copyString(WebServer.arg(CFG_FTP_PASSWORD), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword)); 

    PersistentData.timeZoneOffset = WebServer.arg(CFG_TZ_OFFSET).toInt();
    PersistentData.openThermLogInterval = WebServer.arg(CFG_OT_LOG_INTERVAL).toInt();
    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_SYNC_ENTRIES).toInt();

    copyString(WebServer.arg(CFG_WEATHER_API_KEY), PersistentData.weatherApiKey, sizeof(PersistentData.weatherApiKey));
    copyString(WebServer.arg(CFG_WEATHER_LOC), PersistentData.weatherLocation, sizeof(PersistentData.weatherLocation));

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
