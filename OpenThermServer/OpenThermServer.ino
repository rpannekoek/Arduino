#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <OTGW.h>
#include <Log.h>
#include <WeatherAPI.h>
#include <Wire.h>
#include "PersistentData.h"
#include "OpenThermLogEntry.h"
#include "StatusLogEntry.h"
#include "HeatMonClient.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"
#define SECONDS_PER_DAY (24 * 3600)
#define OTGW_WATCHDOG_INTERVAL 10
#define OTGW_STARTUP_TIME 5
#define OTGW_TIMEOUT 60
#define WIFI_TIMEOUT_MS 2000
#define HTTP_POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define EVENT_LOG_LENGTH 50
#define OTGW_MESSAGE_LOG_LENGTH 40
#define OT_LOG_LENGTH 250
#define OT_LOG_PAGE_SIZE 50
#define KEEP_TSET_LOW_DURATION (15 * 60)
#define WEATHER_SERVICE_POLL_INTERVAL (15 * 60)
#define WEATHER_SERVICE_RESPONSE_TIMEOUT 10
#define FTP_RETRY_INTERVAL (30 * 60)
#define GLOBAL_LOG_INTERVAL (30 * 60)
#define HEATMON_POLL_INTERVAL 60
#define MAX_HEATPUMP_POWER 4

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_FTP_SYNC_ENTRIES F("ftpSyncEntries")
#define CFG_WEATHER_API_KEY F("weatherApiKey")
#define CFG_WEATHER_LOC F("weatherLocation")
#define CFG_MAX_TSET F("maxTSet")
#define CFG_MIN_TSET F("minTSet")
#define CFG_BOILER_ON_DELAY F("boilerOnDelay")
#define CFG_HEATMON_HOST F("heatmonHost")
#define CFG_PUMP_MOD F("pumpMod")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";

const char* BoilerLevelNames[5] = {"Off", "Pump only", "Low", "High", "Thermostat"};

enum BoilerLevel // Unscoped enum so it can be used as array index without casting
{
    Off = 0,
    PumpOnly = 1,
    Low = 2,
    High = 3,
    Thermostat = 4
};

int boilerTSet[5] = {0, 15, 40, 60,  0}; // See initBoilerLevels()

const char* logHeaders[] PROGMEM =
{
    "Time",
    "Status (t)",
    "Status (b)",
    "Max mod",
    "Tset (t)",
    "Tset (b)",
    "Tboiler",
    "Treturn",
    "Tbuffer",
    "Toutside",
    "Pheatpump"
};

OpenThermGateway OTGW(Serial, 14);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
HeatMonClient HeatMon(WIFI_TIMEOUT_MS);
WeatherAPI WeatherService(WIFI_TIMEOUT_MS);
StringBuilder HttpResponse(12288); // 12KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StringLog OTGWMessageLog(OTGW_MESSAGE_LOG_LENGTH, 10);
StaticLog<OpenThermLogEntry> OpenThermLog(OT_LOG_LENGTH);
StaticLog<StatusLogEntry> StatusLog(7); // 7 days
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

// OpenTherm data values indexed by data ID
uint16_t thermostatRequests[256];
uint16_t boilerResponses[256];
uint16_t otgwRequests[256];
uint16_t otgwResponses[256];

time_t watchdogFeedTime = 0;
time_t currentTime = 0;
time_t updateLogTime = 0;
time_t otgwInitializeTime = OTGW_STARTUP_TIME;
time_t otgwTimeout = OTGW_TIMEOUT;
time_t heatmonPollTime = 0;
time_t lastHeatmonUpdateTime = 0;
time_t weatherServicePollTime = 0;
time_t weatherServiceTimeout = 0;
time_t lastWeatherUpdateTime = 0;

bool updateTOutside = false;
bool updateHeatmonData = false;
int lastHeatmonResult = 0;

OpenThermLogEntry newOTLogEntry;
OpenThermLogEntry* lastOTLogEntryPtr = nullptr;
StatusLogEntry* lastStatusLogEntryPtr = nullptr;

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


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


void initBoilerLevels()
{
    boilerTSet[BoilerLevel::Low] = PersistentData.minTSet;
    if (boilerTSet[BoilerLevel::High] != PersistentData.maxTSet)
    {
        boilerTSet[BoilerLevel::High] = PersistentData.maxTSet;
        if (otgwInitializeTime == 0) otgwInitializeTime = currentTime;
    }    
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
    Html.setTitlePrefix(PersistentData.hostName);
    initBoilerLevels();

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/i2c", handleHttpI2cScanRequest);
    WebServer.on("/ot", handleHttpOpenThermRequest);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log", handleHttpOpenThermLogRequest);
    WebServer.on("/log/sync", handleHttpOpenThermLogSyncRequest);
    WebServer.on("/log-csv", handleHttpOpenThermLogCsvRequest);
    WebServer.on("/log-otgw", handleHttpOTGWMessageLogRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/cmd", HTTP_GET, handleHttpCommandFormRequest);
    WebServer.on("/cmd", HTTP_POST, handleHttpCommandFormPost);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
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
    WiFiSM.on(WiFiInitState::Updating, onWiFiUpdating);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    //if (!OTGW.initWatchdog(240)) // 4 minutes timeout (almost max possible)
    //{
    //    logEvent(F("Initializing OTGW Watchdog failed."));
    //}

    //String otgwWatchdogEvent = F("Resets by OTGW Watchdog: ");
    //otgwWatchdogEvent += OTGW.readWatchdogData(14); // TargetResetCount
    //logEvent(otgwWatchdogEvent);

    if (PersistentData.heatmonHost[0] != 0)
    {
        HeatMon.begin(PersistentData.heatmonHost);
    }

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

    if (currentTime >= watchdogFeedTime)
    {
        OTGW.feedWatchdog();
        watchdogFeedTime = currentTime + OTGW_WATCHDOG_INTERVAL;
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
        setOtgwResponse(OpenThermDataId::TOutside, WeatherService.temperature);
        return;
    }

    if (updateHeatmonData)
    {
        updateHeatmonData = false;
        setOtgwResponse(OpenThermDataId::TReturn, HeatMon.tOut);
        setOtgwResponse(OpenThermDataId::Tdhw, HeatMon.tBuffer);
        return;
    }
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
    
    updateLogTime = currentTime + 1;
    heatmonPollTime = currentTime + 10;
}


void onWiFiUpdating()
{
    // Feed the OTGW watchdog just before OTA update
    OTGW.feedWatchdog();
}


void onWiFiInitialized()
{
    if (currentTime >= updateLogTime)
    {
        // Update Status & OT logs every second
        updateLogTime++;
        updateStatusLog(currentTime, boilerResponses[OpenThermDataId::Status]);
        logOpenThermValues(false);
    }

    // Stuff below needs a WiFi connection. If the connection is lost, we skip it.
    if (WiFiSM.getState() < WiFiInitState::Connected)
        return;

    if ((currentTime >= heatmonPollTime) && HeatMon.isInitialized)
    {
        // Get data from HeatMon
        heatmonPollTime = currentTime + HEATMON_POLL_INTERVAL;
        OTGW.feedWatchdog();
        int result = HeatMon.requestData();
        if (result == HTTP_CODE_OK)
        {
            updateHeatmonData = true;
            lastHeatmonUpdateTime = currentTime;
        }
        else if (result != lastHeatmonResult)
        {
            String event = F("HeatMon eror: ");
            event += result;
            logEvent(event);
        }
        lastHeatmonResult = result;
        return;
    }

    if (currentTime >= weatherServicePollTime)
    {
        // Get outside temperature from Weather Service
        weatherServicePollTime = currentTime + WEATHER_SERVICE_POLL_INTERVAL;

        const char* apiKey = PersistentData.weatherApiKey;
        if (apiKey[0] != 0)
        {
            OTGW.feedWatchdog();
            if (WeatherService.beginRequestData(apiKey, PersistentData.weatherLocation))
                weatherServiceTimeout = currentTime + WEATHER_SERVICE_RESPONSE_TIMEOUT;
            else
                TRACE(F("Failed sending Weather service request"));
            return;
        }
    }

    if (weatherServiceTimeout != 0)
    {
        if (currentTime >= weatherServiceTimeout)
        {
            logEvent(F("Weather service timeout"));
            WeatherService.close();
            weatherServiceTimeout = 0;
            return;
        }

        int httpCode = WeatherService.endRequestData();
        if (httpCode != 0)
        {
            weatherServiceTimeout = 0;
            if (httpCode == HTTP_CODE_OK)
            {
                lastWeatherUpdateTime = currentTime;
                float currentTOutside = getDecimal(getResponse(OpenThermDataId::TOutside));
                updateTOutside = (WeatherService.temperature != currentTOutside);
            }
            else
            {
                String event = F("Weather service error: ");
                event += httpCode;
                logEvent(event);
            }
        }
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

    bool success = setOtgwResponse(OpenThermDataId::MaxTSet, boilerTSet[BoilerLevel::High]);

    if (success)
        logEvent(F("OTGW initialized"));
    else
        resetOpenThermGateway();
}


void resetOpenThermGateway()
{
    Tracer tracer(F("resetOpenThermGateway"));

    OTGW.reset();
    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;

    logEvent(F("OTGW reset"));
}


bool setOtgwResponse(OpenThermDataId dataId, float value)
{
    Tracer tracer(F("setOtgwResponse"));
    TRACE(F("dataId: %d, value:%0.1f\n"), dataId, value);

    bool success = OTGW.setResponse(dataId, value); 
    if (!success)
    {
        String event = F("Unable to set OTGW response for #");
        event += dataId; 
        logEvent(event);
    }

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    return setBoilerLevel(level, 0);
}

bool setBoilerLevel(BoilerLevel level, time_t duration)
{
    Tracer tracer(F("setBoilerLevel"));
    TRACE(F("level: %d, duration: %d\n"), level, duration);

    if (duration != 0)
    {
        changeBoilerLevel = BoilerLevel::Thermostat;
        changeBoilerLevelTime = currentTime + duration;
    }

    if (level == currentBoilerLevel)
        return true;

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

        if (currentBoilerLevel == BoilerLevel::Off)
            success = OTGW.sendCommand("CH", "1");
    }

    if (!success)
    {
        logEvent(F("Unable to set boiler level"));
        resetOpenThermGateway();
    }

    currentBoilerLevel = level;

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


uint16_t getResponse(OpenThermDataId dataId)
{
    uint16_t result = otgwResponses[dataId];
    if (result == DATA_VALUE_NONE)
        result = boilerResponses[dataId];
    return result;
}


void updateStatusLog(time_t time, uint16_t status)
{
    if ((lastStatusLogEntryPtr == nullptr) ||
        (time / SECONDS_PER_DAY) > (lastStatusLogEntryPtr->startTime / SECONDS_PER_DAY))
    {
        time_t startOfDay = time - (time % SECONDS_PER_DAY);
        StatusLogEntry* logEntryPtr = new StatusLogEntry();
        logEntryPtr->startTime = startOfDay;
        logEntryPtr->stopTime = startOfDay;
        lastStatusLogEntryPtr = StatusLog.add(logEntryPtr);
        delete logEntryPtr;
    }

    if (status & OpenThermStatus::SlaveCHMode)
    {
        if (lastStatusLogEntryPtr->chSeconds++ == 0)
            lastStatusLogEntryPtr->startTime = time;
        lastStatusLogEntryPtr->stopTime = time;
    }
    if (status & OpenThermStatus::SlaveDHWMode)
        lastStatusLogEntryPtr->dhwSeconds++;
    if (status & OpenThermStatus::SlaveFlame)
        lastStatusLogEntryPtr->flameSeconds++;
}


void logOpenThermValues(bool forceCreate)
{
    newOTLogEntry.time = currentTime;
    newOTLogEntry.thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    newOTLogEntry.thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
    newOTLogEntry.boilerStatus = boilerResponses[OpenThermDataId::Status];
    newOTLogEntry.boilerTSet = boilerResponses[OpenThermDataId::TSet];
    newOTLogEntry.tBoiler = boilerResponses[OpenThermDataId::TBoiler];
    newOTLogEntry.tReturn = getResponse(OpenThermDataId::TReturn);
    newOTLogEntry.tBuffer = getResponse(OpenThermDataId::Tdhw);
    newOTLogEntry.tOutside = getResponse(OpenThermDataId::TOutside);
    newOTLogEntry.pHeatPump = HeatMon.pIn * 256;

    if ((lastOTLogEntryPtr == nullptr) || !newOTLogEntry.equals(lastOTLogEntryPtr) || forceCreate)
    {
        lastOTLogEntryPtr = OpenThermLog.add(&newOTLogEntry);
        if (++otLogEntriesToSync > OT_LOG_LENGTH)
            otLogEntriesToSync = OT_LOG_LENGTH;
        if (otLogEntriesToSync >= PersistentData.ftpSyncEntries)
            otLogSyncTime = currentTime;
    }
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
            OpenThermLogEntry* prevLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync + 1);
            OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync);
            writeCsvDataLines(otLogEntryPtr, prevLogEntryPtr, dataClient);
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
    OTGWMessageLog.add(otgwMessage.message.c_str());

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
        OTGW.feedWatchdog();
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            logEvent(F("Test event"));
            yield();
        }
    }
    else if (message.startsWith("testO"))
    {

        OTGW.feedWatchdog();
        for (int i = 0; i < 50; i++)
        {
            logOpenThermValues(true);
            yield();
        }

        OTGW.feedWatchdog();
        for (int i = 0; i < OTGW_MESSAGE_LOG_LENGTH; i++)
        {
            char testMessage[12];
            snprintf(testMessage, sizeof(testMessage), "T%08X", i);
            OTGWMessageLog.add(testMessage);
        }
    }
    else if (message.startsWith("testW"))
    {
        weatherServicePollTime = currentTime;
    }
    else if (message.startsWith("testS"))
    {
        time_t testTime = currentTime;
        for (int i = 0; i < 7; i++)
        {
            for (int j = 0; j < 90; j++)
            {
                uint16_t testStatus = OpenThermStatus::SlaveCHMode;
                if (j % 2 == 0) testStatus |= OpenThermStatus::SlaveFlame;
                updateStatusLog(testTime, testStatus);
            }
            testTime += SECONDS_PER_DAY;
        }
    }
}


bool handleThermostatLowLoadMode(bool switchedOn)
{
    bool isThermostatLowLoadMode = thermostatRequests[OpenThermDataId::MaxRelModulation] == 0;

    if (isThermostatLowLoadMode)
    {
        if (currentBoilerLevel == BoilerLevel::PumpOnly)
        {
            // Keep Pump Only level, but switch to Low level afterwards.
            changeBoilerLevel = BoilerLevel::Low;
        }
        else
        {
            if (switchedOn || !PersistentData.usePumpModulation)
            {
                // Keep boiler at Low level for a while (prevent on/off modulation)
                setBoilerLevel(BoilerLevel::Low, KEEP_TSET_LOW_DURATION);
            }
            else
            {
                setBoilerLevel(BoilerLevel::Off);
            }
        }
    }

    return isThermostatLowLoadMode;
}


void handleThermostatRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer(F("handleThermostatRequest"));
    
    if (otFrame.dataId == OpenThermDataId::Status)
    {
        bool masterCHEnable = otFrame.dataValue & OpenThermStatus::MasterCHEnable;
        bool lastMasterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
        if (masterCHEnable && !lastMasterCHEnable)
        {
            // Thermostat switched CH on
            if (!handleThermostatLowLoadMode(true) && PersistentData.boilerOnDelay != 0)
            {
                // Keep boiler at Pump Only level for a while (give heat pump a headstart)
                setBoilerLevel(BoilerLevel::PumpOnly, PersistentData.boilerOnDelay);
            }
        }
        else if (!masterCHEnable && lastMasterCHEnable)
        {
            // Thermostat switched CH off
            handleThermostatLowLoadMode(false);
        }        
    }
    else if (otFrame.dataId == OpenThermDataId::MaxRelModulation)
    {
        if (otFrame.dataValue != 0)
        {
            // Thermostat is requesting more than minimal Modulation (no longer in "Low Load Mode").
            if (currentBoilerLevel == BoilerLevel::Low)
            {
                // Let thermostat control boiler TSet again.
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


float getBarValue(float t, float tMin = 20, float tMax = 0)
{
    if (tMax == 0) tMax = boilerTSet[BoilerLevel::High];
    return std::max(t - tMin, 0.0F) / (tMax - tMin);
}


void writeOpenThermTemperatureRow(String label, String cssClass, uint16_t dataValue, float tMin = 20, float tMax = 0)
{
    float value = getDecimal(dataValue);

    HttpResponse.print(F("<tr>"));
    Html.writeHeaderCell(label);
    Html.writeCell(value, F("%0.1f °C"));
    HttpResponse.print(F("<td class=\"graph\">"));
    if (cssClass.length() > 0)
    {
        Html.writeBar(getBarValue(value, tMin, tMax), cssClass, true, false);
    }
    HttpResponse.println(F("</td></tr>"));
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

    String ftpSyncTime;
    if (PersistentData.ftpSyncEntries == 0)
        ftpSyncTime = F("Disabled");
    else if (lastOTLogSyncTime == 0)
        ftpSyncTime = F("Not yet");
    else
        ftpSyncTime = formatTime("%H:%M", lastOTLogSyncTime);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>OpenTherm Gateway status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>RSSI</th><td>%d dBm</td></tr>\r\n"), static_cast<int>(WiFi.RSSI()));
    HttpResponse.printf(F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th>OTGW Errors</th><td>%u</td></tr>\r\n"), otgwErrors);
    HttpResponse.printf(F("<tr><th>OTGW Resets</th><td>%u</td></tr>\r\n"), OTGW.resets);
    HttpResponse.printf(F("<tr><th><a href=\"/log/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSyncTime.c_str());
    HttpResponse.printf(F("<tr><th>FTP sync entries</th><td>%d / %d</td></tr>"), otLogEntriesToSync, PersistentData.ftpSyncEntries);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/log\">OpenTherm log</a></th><td>%d</td></tr>\r\n"), OpenThermLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1><a href=\"/ot\">Current OpenTherm values</a></h1>"));
    if (lastOTLogEntryPtr != nullptr)
    {
        bool flame = boilerResponses[OpenThermDataId::Status] & OpenThermStatus::SlaveFlame;
        float maxRelMod = getDecimal(lastOTLogEntryPtr->thermostatMaxRelModulation);        
        float thermostatTSet = getDecimal(lastOTLogEntryPtr->thermostatTSet);

        time_t overrideTimeLeft = 0;
        if (changeBoilerLevelTime != 0)
        {
            overrideTimeLeft = changeBoilerLevelTime - currentTime;
        }

        HttpResponse.println(F("<table>"));
        HttpResponse.print(F("<tr>"));
        Html.writeHeaderCell(F("Thermostat"));
        if (thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable)
        {
            HttpResponse.printf(
                F("<td>%0.1f °C @ %0.0f %%</td>"),
                thermostatTSet,
                maxRelMod);
        }
        else
            Html.writeCell("CH off");
        HttpResponse.print(F("<td class=\"graph\">"));
        Html.writeBar(getBarValue(thermostatTSet), F("setBar"), true, false);
        HttpResponse.println(F("</td></tr>"));

        writeOpenThermTemperatureRow(F("T<sub>set</sub>"), F("setBar"), lastOTLogEntryPtr->boilerTSet); 
        writeOpenThermTemperatureRow(F("T<sub>boiler</sub>"), flame ? F("flameBar") : F("waterBar"), lastOTLogEntryPtr->tBoiler); 
        writeOpenThermTemperatureRow(F("T<sub>return</sub>"), F("waterBar"), lastOTLogEntryPtr->tReturn); 
        writeOpenThermTemperatureRow(F("T<sub>buffer</sub>"), F("waterBar"), lastOTLogEntryPtr->tBuffer); 
        writeOpenThermTemperatureRow(F("T<sub>outside</sub>"), F("outsideBar"), lastOTLogEntryPtr->tOutside, -10, 30); 

        if (HeatMon.isInitialized)
        {
            HttpResponse.print(F("<tr><th>P<sub>heatpump</sub></th>"));
            Html.writeCell(HeatMon.pIn, F("%0.2f kW"));
            HttpResponse.print(F("<td class=\"graph\">"));
            Html.writeBar(HeatMon.pIn / MAX_HEATPUMP_POWER, F("powerBar"), true, false);
            HttpResponse.println(F("</td></tr>"));
        }

        HttpResponse.printf(
            F("<tr><th>Override</sub></th><td>%s</td><td class=\"graph\">"),
            formatTimeSpan(overrideTimeLeft));
        Html.writeBar(float(overrideTimeLeft) / KEEP_TSET_LOW_DURATION, F("overrideBar"), true, false);
        HttpResponse.println(F("</td></tr>"));
        HttpResponse.println(F("</table>"));
    }

    writeStatisticsPerDay();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeStatisticsPerDay()
{
    HttpResponse.println(F("<h1>Statistics per day</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.print(F("<tr>"));
    Html.writeHeaderCell(F("Day"));
    Html.writeHeaderCell(F("CH start"));
    Html.writeHeaderCell(F("CH stop"));
    Html.writeHeaderCell(F("CH duration"));
    Html.writeHeaderCell(F("DHW  duration"));
    Html.writeHeaderCell(F("Flame duration"));
    HttpResponse.println(F("</tr>"));
    uint32_t maxFlameSeconds = getMaxFlameSeconds() + 1; // Prevent division by zero
    StatusLogEntry* logEntryPtr = StatusLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        HttpResponse.print(F("<tr>"));
        Html.writeCell(formatTime("%a", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->stopTime));
        Html.writeCell(formatTimeSpan(logEntryPtr->chSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->dhwSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->flameSeconds));
        HttpResponse.print(F("<td class=\"graph\">"));
        Html.writeBar(float(logEntryPtr->flameSeconds) / maxFlameSeconds, F("flameBar"), false, false);
        HttpResponse.println(F("</td></tr>"));

        logEntryPtr = StatusLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));
}


uint32_t getMaxFlameSeconds()
{
    uint32_t result = 0;
    StatusLogEntry* logEntryPtr = StatusLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        result = std::max(result, logEntryPtr->flameSeconds);
        logEntryPtr = StatusLog.getNextEntry();
    }
    return result;
}


void handleHttpI2cScanRequest()
{
    Html.writeHeader(F("I2C scan"), true, true);

    HttpResponse.println(F("<table>"));

    for (int i = 1; i < 127; i++)
    {
        Wire.beginTransmission(i);
        uint8_t err = Wire.endTransmission();

        HttpResponse.printf(
            F("<tr><td>%d</td><td>%d</td></tr>\r\n"),
            i,
            err);
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
        avgBurnerOnTime =  float(boilerResponses[OpenThermDataId::BoilerBurnerHours] * 3600)  
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
    HttpResponse.printf(F("<tr><td>Fault flags</td><td>%s</td></tr>\r\n"), OTGW.getFaultFlags(boilerResponses[OpenThermDataId::SlaveFault]));
    HttpResponse.printf(F("<tr><td>Burner starts</td><td>%d</td></tr>\r\n"), burnerStarts);
    HttpResponse.printf(F("<tr><td>Burner on</td><td>%d h</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    HttpResponse.printf(F("<tr><td>Burner on DHW</td><td>%d h</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerDHWBurnerHours]);
    HttpResponse.printf(F("<tr><td>Avg burner on</td><td>%s</td></tr>\r\n"), formatTimeSpan(avgBurnerOnTime));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Boiler override</h1>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Current level</td><td>%s</td></tr>\r\n"), BoilerLevelNames[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        HttpResponse.printf(F("<tr><td>Change to</td><td>%s</td></tr>\r\n"), BoilerLevelNames[changeBoilerLevel]);
        HttpResponse.printf(F("<tr><td>Change at</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", changeBoilerLevelTime));
    }
    HttpResponse.printf(F("<tr><td>Override duration</td><td>%s</td></tr>\r\n"), formatTimeSpan(totalOverrideDuration));
    if (lastHeatmonUpdateTime != 0)
        HttpResponse.printf(F("<tr><td>HeatMon update</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastHeatmonUpdateTime));
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

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((OpenThermLog.count() - 1) / OT_LOG_PAGE_SIZE) + 1;
    HttpResponse.print(F("<p>Pages: "));
    for (int i = 0; i < totalPages; i++)
    {
        if (i > 0)
            HttpResponse.print(F(" | "));
        if (i == currentPage)
            HttpResponse.printf(F("%d"), i + 1);
        else
            HttpResponse.printf(F("<a href='?page=%d'>%d</a>"), i, i + 1);           
    }
    HttpResponse.println(F("</p>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.print(F("<tr>"));
    for (PGM_P header : logHeaders)
    {
        Html.writeHeaderCell(FPSTR(header));
    }
    HttpResponse.println(F("</tr>"));

    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    for (int i = 0; i < (currentPage * OT_LOG_PAGE_SIZE) && otLogEntryPtr != nullptr; i++)
    {
        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
    for (int j = 0; j < OT_LOG_PAGE_SIZE && otLogEntryPtr != nullptr; j++)
    {
        HttpResponse.print(F("<tr>"));
        Html.writeCell(formatTime("%H:%M:%S", otLogEntryPtr->time));
        Html.writeCell(OTGW.getMasterStatus(otLogEntryPtr->boilerStatus));
        Html.writeCell(OTGW.getSlaveStatus(otLogEntryPtr->boilerStatus));
        Html.writeCell(getInteger(otLogEntryPtr->thermostatMaxRelModulation));
        Html.writeCell(getInteger(otLogEntryPtr->thermostatTSet));
        Html.writeCell(getInteger(otLogEntryPtr->boilerTSet));
        Html.writeCell(getDecimal(otLogEntryPtr->tBoiler));
        Html.writeCell(getDecimal(otLogEntryPtr->tReturn));
        Html.writeCell(getDecimal(otLogEntryPtr->tBuffer));
        Html.writeCell(getDecimal(otLogEntryPtr->tOutside));
        Html.writeCell(getDecimal(otLogEntryPtr->pHeatPump), F("%0.2f"));
        HttpResponse.println(F("</tr>"));

        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    TRACE(F("Response size: %u\n"), strlen(HttpResponse));

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

    HttpResponse.clear();
    bool first = true;
    for (PGM_P header : logHeaders)
    {
        if (first)
            first = false;
        else
            HttpResponse.print(";");
        HttpResponse.print(FPSTR(header));
    }
    HttpResponse.println();

    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    writeCsvDataLines(otLogEntryPtr, nullptr, HttpResponse);

    WebServer.send(200, ContentTypeText, HttpResponse);
}


void writeCsvDataLines(OpenThermLogEntry* otLogEntryPtr, OpenThermLogEntry* prevLogEntryPtr, Print& destination)
{
    while (otLogEntryPtr != nullptr)
    {
        time_t otLogEntryTime = otLogEntryPtr->time;
        time_t oneSecEarlier = otLogEntryTime - 1;
        if ((prevLogEntryPtr != nullptr) && (prevLogEntryPtr->time < oneSecEarlier))
        {
            // Repeat previous log entry, but one second before this one.
            // This enforces steep step transitions.
            writeCsvDataLine(prevLogEntryPtr, oneSecEarlier, destination);
        }
        writeCsvDataLine(otLogEntryPtr, otLogEntryTime, destination);
        
        prevLogEntryPtr = otLogEntryPtr;
        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
}


void writeCsvDataLine(OpenThermLogEntry* otLogEntryPtr, time_t time, Print& destination)
{
    int masterStatus = otLogEntryPtr->boilerStatus >> 8;
    int slaveStatus = otLogEntryPtr->boilerStatus & 0xFF;

    destination.print(formatTime("%F %H:%M:%S", time));
    destination.printf(";%d;%d", masterStatus, slaveStatus);
    destination.printf(";%d", getInteger(otLogEntryPtr->thermostatMaxRelModulation));
    destination.printf(";%d", getInteger(otLogEntryPtr->thermostatTSet));
    destination.printf(";%d", getInteger(otLogEntryPtr->boilerTSet));
    destination.printf(";%0.1f", getDecimal(otLogEntryPtr->tBoiler));
    destination.printf(";%0.1f", getDecimal(otLogEntryPtr->tReturn));
    destination.printf(";%0.1f", getDecimal(otLogEntryPtr->tBuffer));
    destination.printf(";%0.1f", getDecimal(otLogEntryPtr->tOutside));
    destination.printf(";%0.2f\r\n", getDecimal(otLogEntryPtr->pHeatPump));
}


void handleHttpOTGWMessageLogRequest()
{
    Tracer tracer(F("handleHttpOTGWMessageLogRequest"));

    HttpResponse.clear();

    const char* otgwMessage = OTGWMessageLog.getFirstEntry();
    while (otgwMessage != nullptr)
    {
        HttpResponse.println(otgwMessage);
        otgwMessage = OTGWMessageLog.getNextEntry();
    }

    OTGWMessageLog.clear();

    WebServer.send(200, ContentTypeText, HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (WiFiSM.shouldPerformAction(F("clear")))
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
    Html.writeTextBox(CFG_HEATMON_HOST, F("Heatmon host"), PersistentData.heatmonHost, sizeof(PersistentData.heatmonHost) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1);
    Html.writeTextBox(CFG_FTP_SYNC_ENTRIES, F("FTP Sync Entries"), String(PersistentData.ftpSyncEntries), 4);
    Html.writeTextBox(CFG_WEATHER_API_KEY, F("Weather API Key"), PersistentData.weatherApiKey, 16);
    Html.writeTextBox(CFG_WEATHER_LOC, F("Weather Location"), PersistentData.weatherLocation, 16);
    Html.writeTextBox(CFG_MAX_TSET, F("Max TSet"), String(PersistentData.maxTSet), 2);
    Html.writeTextBox(CFG_MIN_TSET, F("Min TSet"), String(PersistentData.minTSet), 2);
    Html.writeTextBox(CFG_BOILER_ON_DELAY, F("Boiler on delay (s)"), String(PersistentData.boilerOnDelay), 4);
    Html.writeCheckbox(CFG_PUMP_MOD, F("Pump Modulation"), PersistentData.usePumpModulation);
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

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
    copyString(WebServer.arg(CFG_HEATMON_HOST), PersistentData.heatmonHost, sizeof(PersistentData.heatmonHost)); 

    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_SYNC_ENTRIES).toInt();

    copyString(WebServer.arg(CFG_WEATHER_API_KEY), PersistentData.weatherApiKey, sizeof(PersistentData.weatherApiKey));
    copyString(WebServer.arg(CFG_WEATHER_LOC), PersistentData.weatherLocation, sizeof(PersistentData.weatherLocation));

    PersistentData.maxTSet = WebServer.arg(CFG_MAX_TSET).toInt();
    PersistentData.minTSet = WebServer.arg(CFG_MIN_TSET).toInt();
    PersistentData.boilerOnDelay = WebServer.arg(CFG_BOILER_ON_DELAY).toInt();
    PersistentData.usePumpModulation = WebServer.hasArg(CFG_PUMP_MOD);

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    initBoilerLevels();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
