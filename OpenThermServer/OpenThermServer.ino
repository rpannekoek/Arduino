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
#include <Navigation.h>
#include <HtmlWriter.h>
#include "OTGW.h"
#include <Log.h>
#include "WeatherAPI.h"
#include <Wire.h>
#include "PersistentData.h"
#include "OpenThermLogEntry.h"
#include "StatusLogEntry.h"
#include "HeatMonClient.h"

#define SECONDS_PER_DAY (24 * 3600)
#define SET_BOILER_RETRY_INTERVAL 6
#define OTGW_WATCHDOG_INTERVAL 10
#define OTGW_STARTUP_TIME 5
#define WIFI_TIMEOUT_MS 2000
#define HTTP_POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define EVENT_LOG_LENGTH 50
#define OTGW_MESSAGE_LOG_LENGTH 40
#define OT_LOG_LENGTH 250
#define OT_LOG_PAGE_SIZE 50
#define PWM_PERIOD (10 * 60)
#define TSET_OVERRIDE_DURATION (15 * 60)
#define WEATHER_SERVICE_POLL_INTERVAL (15 * 60)
#define WEATHER_SERVICE_RESPONSE_TIMEOUT 10
#define FTP_RETRY_INTERVAL (15 * 60)
#define HEATMON_POLL_INTERVAL 60
#define MAX_HEATPUMP_POWER 4
#define MAX_PRESSURE 3

#ifdef DEBUG_ESP_PORT
    #define OTGW_TIMEOUT (5 * 60)
    #define OTGW_RESPONSE_TIMEOUT_MS 5000
#else
    #define OTGW_TIMEOUT 60
    #define OTGW_RESPONSE_TIMEOUT_MS 2000
#endif

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

enum FileId
{
    Logo,
    Styles,
    Home,
    Calibrate,
    Graph,
    LogFile,
    Settings,
    Upload,
    _Last
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "Calibrate.svg",
    "Graph.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};


const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

const char* BoilerLevelNames[5] = {"Off", "Pump-only", "Low", "High", "Thermostat"};

enum BoilerLevel // Unscoped enum so it can be used as array index without casting
{
    Off = 0,
    PumpOnly = 1,
    Low = 2,
    High = 3,
    Thermostat = 4
};

int boilerTSet[5] = {0, 15, 40, 60,  0}; // See initBoilerLevels()

const char* LogHeaders[] PROGMEM =
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
    "Pheatpump",
    "Pressure",
    "Mod (%)"
};

OpenThermGateway OTGW(Serial, 14, OTGW_RESPONSE_TIMEOUT_MS);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
HeatMonClient HeatMon(WIFI_TIMEOUT_MS);
WeatherAPI WeatherService(WIFI_TIMEOUT_MS);
StringBuilder HttpResponse(12 * 1024); // 12KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[FileId::Logo], Files[FileId::Styles], 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StringLog OTGWMessageLog(OTGW_MESSAGE_LOG_LENGTH, 10);
StaticLog<OpenThermLogEntry> OpenThermLog(OT_LOG_LENGTH);
StaticLog<StatusLogEntry> StatusLog(7); // 7 days
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Navigation Nav;

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

time_t lowLoadLastOn = 0;
time_t lowLoadLastOff = 0;
uint32_t lowLoadPeriod = 0;
uint32_t lowLoadDutyInterval = 0;

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
int setBoilerLevelRetries = 0; 
float pwmDutyCycle = 1;

String otgwResponse;


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
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
    Html.setTitlePrefix(PersistentData.hostName);
    initBoilerLevels();

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));
    memset(otgwRequests, 0xFF, sizeof(otgwRequests));
    memset(otgwResponses, 0xFF, sizeof(otgwResponses));

    if (SPIFFS.begin())
        WiFiSM.registerStaticFiles(Files, FileId::_Last);
    else
        WiFiSM.logEvent(F("Failed starting SPIFFS"));

    Nav.width = F("12em");
    Nav.menuItems =
    {
        MenuItem 
        {
            .icon = Files[FileId::Home],
            .label = PSTR("Home"),
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::LogFile],
            .label = PSTR("Event log"),
            .urlPath = PSTR("events"),
            .handler = handleHttpEventLogRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Graph],
            .label = PSTR("OpenTherm log"),
            .urlPath = PSTR("otlog"),
            .handler = handleHttpOpenThermLogRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Logo],
            .label = PSTR("OpenTherm data"),
            .urlPath = PSTR("ot"),
            .handler = handleHttpOpenThermRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Upload],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler = handleHttpOpenThermLogSyncRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Calibrate],
            .label = PSTR("OTGW Command"),
            .urlPath = PSTR("cmd"),
            .handler = handleHttpCommandFormRequest,
            .postHandler = handleHttpCommandFormPost            
        },
        MenuItem
        {
            .icon = Files[FileId::Settings],
            .label = PSTR("Settings"),
            .urlPath = PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost            
        }
    };

    Nav.registerHttpHandlers(WebServer);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log-otgw", handleHttpOTGWMessageLogRequest);
    WebServer.onNotFound(handleHttpNotFound);

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
        WiFiSM.logEvent(F("OTGW Timeout"));
        resetOpenThermGateway();
        return;
    }

    if ((otgwInitializeTime != 0) && (currentTime >= otgwInitializeTime))
    {
        otgwInitializeTime = 0;
        initializeOpenThermGateway();
        return;
    }

    // Scheduled Boiler TSet change (incl. forced PWM)
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        changeBoilerLevelTime = 0;
        if (pwmDutyCycle < 1)
        {
            if (currentBoilerLevel == BoilerLevel::Off)
                setBoilerLevel(BoilerLevel::Low, pwmDutyCycle * PWM_PERIOD);
            else if (currentBoilerLevel == BoilerLevel::Low)
                setBoilerLevel(BoilerLevel::Off, (1.0F - pwmDutyCycle) * PWM_PERIOD);
            else
            {
                WiFiSM.logEvent(F("Unexpected level for PWM: %s"), BoilerLevelNames[currentBoilerLevel]);
                cancelOverride();
            }
        }
        else if (changeBoilerLevel == BoilerLevel::Thermostat)
            cancelOverride();
        else
        {
            WiFiSM.logEvent(
                F("Override %s changed to %s"),
                BoilerLevelNames[currentBoilerLevel],
                BoilerLevelNames[changeBoilerLevel]);
            setBoilerLevel(changeBoilerLevel, TSET_OVERRIDE_DURATION);
        }
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
            WiFiSM.logEvent(F("HeatMon: %s"), HeatMon.getLastError().c_str());
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
            WiFiSM.logEvent(F("Weather service timeout"));
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
                WiFiSM.logEvent(F("Weather service error %d"), httpCode);
            }
        }
    }

    if ((otLogSyncTime != 0) && (currentTime >= otLogSyncTime))
    {
        if (trySyncOpenThermLog(nullptr))
        {
            WiFiSM.logEvent(F("FTP sync"));
            otLogSyncTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
            otLogSyncTime = currentTime + FTP_RETRY_INTERVAL;
        }
    }
}


void initializeOpenThermGateway()
{
    Tracer tracer(F("initializeOpenThermGateway"));

    bool success = setOtgwResponse(OpenThermDataId::MaxTSet, boilerTSet[BoilerLevel::High]);

    if (success)
        WiFiSM.logEvent(F("OTGW initialized"));
    else
        resetOpenThermGateway();
}


void resetOpenThermGateway()
{
    Tracer tracer(F("resetOpenThermGateway"));

    OTGW.reset();

    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;

    currentBoilerLevel = BoilerLevel::Thermostat;

    WiFiSM.logEvent(F("OTGW reset"));
}


bool setOtgwResponse(OpenThermDataId dataId, float value)
{
    Tracer tracer(F("setOtgwResponse"));
    TRACE(F("dataId: %d, value:%0.1f\n"), dataId, value);

    bool success = OTGW.setResponse(dataId, value); 
    if (!success)
    {
        WiFiSM.logEvent(F("Unable to set OTGW response for #%d"), dataId);
    }

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    return setBoilerLevel(level, 0);
}

bool setBoilerLevel(BoilerLevel level, time_t duration)
{
    Tracer tracer(F("setBoilerLevel"), BoilerLevelNames[level]);
    TRACE(F("Duration: %d\n"), static_cast<int>(duration));

    if (duration != 0)
    {
        changeBoilerLevel = BoilerLevel::Thermostat;
        changeBoilerLevelTime = currentTime + duration;
    }

    if (level == currentBoilerLevel)
        return true;

    if ((changeBoilerLevelTime != 0) && (level == changeBoilerLevel))
        changeBoilerLevelTime = 0;

    bool success;
    if (level == BoilerLevel::Off)
        success = OTGW.sendCommand("CH", "0");
    else
    {
        success = OTGW.sendCommand("CS", String(boilerTSet[level]));

        if (success && (currentBoilerLevel == BoilerLevel::Off))
            success = OTGW.sendCommand("CH", "1");
    }

    if (success)
    {
        currentBoilerLevel = level;
        setBoilerLevelRetries = 0;
    }
    else if (++setBoilerLevelRetries < 3)
    {
        changeBoilerLevel = level;
        changeBoilerLevelTime = WiFiSM.getCurrentTime() + SET_BOILER_RETRY_INTERVAL;
        WiFiSM.logEvent(
            F("Unable to set boiler to %s. Retry %d at %s."),
            BoilerLevelNames[level],
            setBoilerLevelRetries,
            formatTime("%H:%M:%S", changeBoilerLevelTime));
    }
    else
    {
        WiFiSM.logEvent(
            F("Unable to set boiler to %s after %d attempts."),
            BoilerLevelNames[level],
            setBoilerLevelRetries);
        setBoilerLevelRetries = 0;
        resetOpenThermGateway();
    }

    return success;
}


void cancelOverride()
{
    pwmDutyCycle = 1;
    lowLoadPeriod = 0;
    lowLoadDutyInterval = 0;
    WiFiSM.logEvent(F("Override %s stopped"), BoilerLevelNames[currentBoilerLevel]);
    setBoilerLevel(BoilerLevel::Thermostat);
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

    if (currentBoilerLevel != BoilerLevel::Thermostat)
        lastStatusLogEntryPtr->overrideSeconds++;
}


void logOpenThermValues(bool forceCreate)
{
    newOTLogEntry.time = currentTime;
    newOTLogEntry.thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    newOTLogEntry.thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
    newOTLogEntry.boilerStatus = boilerResponses[OpenThermDataId::Status];
    newOTLogEntry.boilerTSet = boilerResponses[OpenThermDataId::TSet];
    newOTLogEntry.boilerRelModulation = boilerResponses[OpenThermDataId::RelModulation];
    newOTLogEntry.tBoiler = boilerResponses[OpenThermDataId::TBoiler];
    newOTLogEntry.tReturn = getResponse(OpenThermDataId::TReturn);
    newOTLogEntry.tBuffer = getResponse(OpenThermDataId::Tdhw);
    newOTLogEntry.tOutside = getResponse(OpenThermDataId::TOutside);
    newOTLogEntry.pressure = boilerResponses[OpenThermDataId::Pressure];
    newOTLogEntry.pHeatPump = HeatMon.pIn * 256;

    if ((lastOTLogEntryPtr == nullptr) || !newOTLogEntry.equals(lastOTLogEntryPtr) || forceCreate)
    {
        lastOTLogEntryPtr = OpenThermLog.add(&newOTLogEntry);
        otLogEntriesToSync = std::min(otLogEntriesToSync + 1, OT_LOG_LENGTH);
        if (PersistentData.isFTPEnabled() && otLogEntriesToSync == PersistentData.ftpSyncEntries)
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
        return false;
    }

    bool success = false;
    WiFiClient& dataClient = FTPClient.append("OTGW.csv");
    if (dataClient.connected())
    {
        if (otLogEntriesToSync > 0)
        {
            OpenThermLogEntry* prevLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync + 1);
            OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync);
            writeCsvDataLines(otLogEntryPtr, prevLogEntryPtr, dataClient);
            otLogEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println(F("Nothing to sync."));
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastOTLogSyncTime = currentTime;
            success = true;
        }
        else
            FTPClient.setUnexpectedResponse();
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
            WiFiSM.logEvent(F("OTGW: '%s'"), otgwMessage.message.c_str());
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
            WiFiSM.logEvent(F("Test event"));
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
            if (changeBoilerLevel == BoilerLevel::Thermostat)
            {
                changeBoilerLevel = BoilerLevel::Low;
                WiFiSM.logEvent(F("Start low-load at %s"), formatTime("%H:%M", changeBoilerLevelTime));
            }
        }
        else
        {
            if (currentBoilerLevel == BoilerLevel::Thermostat || pwmDutyCycle < 1)
            {
                pwmDutyCycle = 1;
                WiFiSM.logEvent(F("Low-load started"));
            }

            BoilerLevel newBoilerLevel = BoilerLevel::Low;
            if (switchedOn)
            {
                if (lowLoadLastOn != 0)
                    lowLoadPeriod = currentTime - lowLoadLastOn;
                lowLoadLastOn = currentTime;
            }
            else
            {
                if (lowLoadLastOn != 0)
                    lowLoadDutyInterval = currentTime - lowLoadLastOn;
                lowLoadLastOff = currentTime;

                if (PersistentData.usePumpModulation && currentBoilerLevel != BoilerLevel::Thermostat)
                {
                    // Thermostat switched CH off and pump modulation is on.
                    // Switch CH off, but keep the TSet override (for a while). 
                    newBoilerLevel = BoilerLevel::Off;
                }
            }
            setBoilerLevel(newBoilerLevel, TSET_OVERRIDE_DURATION);
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
            if (!handleThermostatLowLoadMode(true))
            {
                // Thermostat is not in low load mode
                if (currentBoilerLevel != BoilerLevel::Thermostat)
                    cancelOverride();
                else if (PersistentData.boilerOnDelay != 0)
                {
                    // Keep boiler at Pump-only level for a while (give heatpump a headstart)
                    if (setBoilerLevel(BoilerLevel::PumpOnly, PersistentData.boilerOnDelay))
                        WiFiSM.logEvent(F("Pump-only until %s"), formatTime("%H:%M", changeBoilerLevelTime));
                }
            }
        }
        else if (!masterCHEnable && lastMasterCHEnable)
        {
            // Thermostat switched CH off
            if (!handleThermostatLowLoadMode(false))
            {
                // Thermostat is not in low load mode
                if (currentBoilerLevel != BoilerLevel::Thermostat)
                    cancelOverride();
            }
        }        
    }
    else if (otFrame.dataId == OpenThermDataId::MaxRelModulation)
    {
        if (otFrame.dataValue != 0 && thermostatRequests[OpenThermDataId::MaxRelModulation] == 0)
        {
            // Thermostat started requesting more than minimal Modulation (no longer in low-load mode).
            // If we're still in low-load mode and CH is on, we cancel the override here.
            // If the CH is off, it will be cancelled later when CH is switched on again.
            if (currentBoilerLevel == BoilerLevel::Low && pwmDutyCycle == 1)
                cancelOverride();
        }
    }
    else if (otFrame.dataId == OpenThermDataId::TSet)
    {
        bool masterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
        if (masterCHEnable && PersistentData.usePumpModulation && currentBoilerLevel != BoilerLevel::PumpOnly)
        {
            bool pwmPending = pwmDutyCycle < 1;
            float tSet = getDecimal(otFrame.dataValue);
            float tBoiler = getDecimal(boilerResponses[OpenThermDataId::TBoiler]);
            float tPwmCeiling = std::min(std::max(tBoiler, (float)PersistentData.minTSet), (float)PersistentData.maxTSet);
            const float tPwmFloor = 20;
            if (tSet >= tPwmFloor && tSet < tPwmCeiling)
            {
                // Requested TSet in PWM range; use PWM.
                pwmDutyCycle = (tSet - tPwmFloor) / (tPwmCeiling - tPwmFloor);
                pwmDutyCycle = std::min(std::max(pwmDutyCycle, 0.1F), 0.9F); // Keep between 10% and 90%
                if (!pwmPending)
                {
                    WiFiSM.logEvent(F("PWM started: %0.0f%%. TSet=%0.1f < %0.1f"), pwmDutyCycle * 100, tSet, tPwmCeiling);
                    setBoilerLevel(BoilerLevel::Low, pwmDutyCycle * PWM_PERIOD);
                }
            }
            else if (pwmPending)
            {
                // Requested TSet is below 15 or above configured minimum; cancel PWM.
                WiFiSM.logEvent(F("PWM stopped. TSet=%0.1f"), tSet);
                cancelOverride();
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
            WiFiSM.logEvent(F("Max CH Water setpoint is changed (by OTGW reset?)"));
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
    float barValue = getBarValue(value, tMin, tMax);

    Html.writeRowStart();
    Html.writeHeaderCell(label);
    Html.writeCell(value, F("%0.1f °C"));
    Html.writeGraphCell(barValue, cssClass, true);
    Html.writeRowEnd();
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
    if (!PersistentData.isFTPEnabled())
        ftpSyncTime = F("Disabled");
    else if (lastOTLogSyncTime == 0)
        ftpSyncTime = F("Not yet");
    else
        ftpSyncTime = formatTime("%H:%M", lastOTLogSyncTime);

    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("OTGW Errors"), F("%u"), otgwErrors);
    Html.writeRow(F("OTGW Resets"), F("%u"), OTGW.resets);
    Html.writeRow(F("FTP Sync"), F("%s"), ftpSyncTime.c_str());
    Html.writeRow(F("Sync entries"), F("%d / %d"), otLogEntriesToSync, PersistentData.ftpSyncEntries);
    if (lastHeatmonUpdateTime != 0)
        Html.writeRow(F("HeatMon"), F("%s"), formatTime("%H:%M", lastHeatmonUpdateTime));
    if (lastWeatherUpdateTime != 0)
        Html.writeRow(F("Weather"), F("%s"), formatTime("%H:%M", lastWeatherUpdateTime));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeCurrentValues();
    writeStatisticsPerDay();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}

void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();

    if (lastOTLogEntryPtr != nullptr)
    {
        bool flame = boilerResponses[OpenThermDataId::Status] & OpenThermStatus::SlaveFlame;
        float maxRelMod = getDecimal(lastOTLogEntryPtr->thermostatMaxRelModulation);
        float relMod = getDecimal(lastOTLogEntryPtr->boilerRelModulation);        
        float thermostatTSet = getDecimal(lastOTLogEntryPtr->thermostatTSet);
        float boilerTset = getDecimal(lastOTLogEntryPtr->boilerTSet);

        Html.writeRowStart();
        Html.writeHeaderCell(F("Thermostat"));
        if (thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable)
            Html.writeCell(F("%0.1f °C @ %0.0f %%"), thermostatTSet, maxRelMod);
        else
            Html.writeCell(F("CH off"));
        Html.writeGraphCell(getBarValue(thermostatTSet), F("setBar"), true);
        Html.writeRowEnd();

        Html.writeRowStart();
        Html.writeHeaderCell(F("Boiler"));
        if (boilerResponses[OpenThermDataId::Status] & 0xFF)
            Html.writeCell(F("%0.1f °C @ %0.0f %%"), boilerTset, relMod);
        else
            Html.writeCell(F("Off"));
        Html.writeGraphCell(getBarValue(boilerTset), F("setBar"), true);
        Html.writeRowEnd();

        writeOpenThermTemperatureRow(F("T<sub>boiler</sub>"), flame ? F("flameBar") : F("waterBar"), lastOTLogEntryPtr->tBoiler); 
        writeOpenThermTemperatureRow(F("T<sub>return</sub>"), F("waterBar"), lastOTLogEntryPtr->tReturn); 
        writeOpenThermTemperatureRow(F("T<sub>buffer</sub>"), F("waterBar"), lastOTLogEntryPtr->tBuffer); 
        writeOpenThermTemperatureRow(F("T<sub>outside</sub>"), F("outsideBar"), lastOTLogEntryPtr->tOutside, -10, 30);

        float pressure = getDecimal(lastOTLogEntryPtr->pressure);
        Html.writeRowStart();
        Html.writeHeaderCell(F("Pressure"));
        Html.writeCell(pressure, F("%0.2f bar"));
        Html.writeGraphCell(pressure / MAX_PRESSURE, F("pressureBar"), true);
        Html.writeRowEnd();
    }

    if (HeatMon.isInitialized)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(F("P<sub>heatpump</sub>"));
        Html.writeCell(HeatMon.pIn, F("%0.2f kW"));
        Html.writeGraphCell(HeatMon.pIn / MAX_HEATPUMP_POWER, F("powerBar"), true);
        Html.writeRowEnd();
    }

    time_t overrideTimeLeft =(changeBoilerLevelTime == 0) ? 0 : changeBoilerLevelTime - currentTime;
    const char* duration = (overrideTimeLeft == 0) ? "" : formatTimeSpan(overrideTimeLeft, false); 
    Html.writeRowStart();
    Html.writeHeaderCell(F("Override"));
    Html.writeCell(F("%s %s"), BoilerLevelNames[currentBoilerLevel], duration);
    Html.writeGraphCell(float(overrideTimeLeft) / TSET_OVERRIDE_DURATION, F("overrideBar"), true);
    Html.writeRowEnd();

    if (lowLoadPeriod != 0 || pwmDutyCycle < 1)
    {
        float dutyCycle = (pwmDutyCycle < 1) ? pwmDutyCycle : float(lowLoadDutyInterval) / lowLoadPeriod;
        const char* pwmType = (pwmDutyCycle < 1) ? "Forced" : "Low-load";

        Html.writeRowStart();
        Html.writeHeaderCell(F("PWM"));
        Html.writeCell(F("%s %0.0f %%"), pwmType, dutyCycle * 100);
        Html.writeGraphCell(dutyCycle, F("overrideBar"), true);
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void writeStatisticsPerDay()
{
    Html.writeSectionStart(F("Statistics per day"));

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Day"));
    Html.writeHeaderCell(F("CH start"));
    Html.writeHeaderCell(F("CH stop"));
    Html.writeHeaderCell(F("Override"));
    Html.writeHeaderCell(F("CH on"));
    Html.writeHeaderCell(F("DHW on"));
    Html.writeHeaderCell(F("Flame"));
    Html.writeRowEnd();

    uint32_t maxFlameSeconds = getMaxFlameSeconds() + 1; // Prevent division by zero
    StatusLogEntry* logEntryPtr = StatusLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%a", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->stopTime));
        Html.writeCell(formatTimeSpan(logEntryPtr->overrideSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->chSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->dhwSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->flameSeconds));
        Html.writeCellStart(F("graph"));
        Html.writeBar(float(logEntryPtr->flameSeconds) / maxFlameSeconds, F("flameBar"), false, false);
        Html.writeCellEnd();
        Html.writeRowEnd();

        logEntryPtr = StatusLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
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


void handleHttpOpenThermRequest()
{
    Tracer tracer(F("handleHttpOpenThermRequest"));

    uint16_t burnerStarts = boilerResponses[OpenThermDataId::BoilerBurnerStarts];
    float avgBurnerOnTime;
    if ((burnerStarts == DATA_VALUE_NONE) || (burnerStarts == 0))
        avgBurnerOnTime = 0.0;
    else
    {
        int totalBurnerHours = boilerResponses[OpenThermDataId::BoilerBurnerHours] 
            + boilerResponses[OpenThermDataId::BoilerDHWBurnerHours];
        avgBurnerOnTime =  float(totalBurnerHours * 3600) / burnerStarts; 
    }

    Html.writeHeader(F("OpenTherm data"), Nav);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Thermostat"));
    Html.writeTableStart();
    Html.writeRow(F("Status"), F("%s"), OTGW.getMasterStatus(thermostatRequests[OpenThermDataId::Status]));
    Html.writeRow(F("TSet"), F("%0.1f"), getDecimal(thermostatRequests[OpenThermDataId::TSet]));
    Html.writeRow(F("Max Modulation %"), F("%0.1f"), getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation]));
    Html.writeRow(F("Max TSet"), F("%0.1f"), getDecimal(thermostatRequests[OpenThermDataId::MaxTSet]));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Boiler"));
    Html.writeTableStart();
    Html.writeRow(F("Status"), F("%s"), OTGW.getSlaveStatus(boilerResponses[OpenThermDataId::Status]));
    Html.writeRow(F("TSet"), F("%0.1f"), getDecimal(boilerResponses[OpenThermDataId::TSet]));
    Html.writeRow(F("Fault flags"), F("%s"), OTGW.getFaultFlags(boilerResponses[OpenThermDataId::SlaveFault]));
    Html.writeRow(F("Burner starts"), F("%d"), burnerStarts);
    Html.writeRow(F("Burner on"), F("%d h"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    Html.writeRow(F("Avg burner on"), F("%s"), formatTimeSpan(avgBurnerOnTime));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Boiler override"));
    Html.writeTableStart();
    Html.writeRow(F("Current level"), F("%s"), BoilerLevelNames[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        Html.writeRow(F("Change to"), F("%s"), BoilerLevelNames[changeBoilerLevel]);
        Html.writeRow(F("Change at"), F("%s"), formatTime("%H:%M", changeBoilerLevelTime));
    }
    if (lowLoadPeriod != 0)
        Html.writeRow(F("Low-load period"), F("%s"), formatTimeSpan(lowLoadPeriod, false));
    if (lowLoadDutyInterval != 0)
        Html.writeRow(F("Low-load duty"), F("%s"), formatTimeSpan(lowLoadDutyInterval, false));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeDivEnd();

    Html.writeLink(F("/traffic"), F("View all OpenTherm traffic"), ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void writeHtmlOpenThermDataTable(const String& title, uint16_t* otDataTable)
{
    Html.writeSectionStart(title);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("ID"));
    Html.writeHeaderCell(F("Hex value"));
    Html.writeHeaderCell(F("Dec value"));
    Html.writeRowEnd();

    for (int i = 0; i < 256; i++)
    {
        uint16_t dataValue = otDataTable[i];
        if (dataValue == DATA_VALUE_NONE) continue;
        Html.writeRowStart();
        Html.writeCell(i);
        Html.writeCell(F("%04X"), dataValue);
        Html.writeCell(F("%0.2f"), getDecimal(dataValue));
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpOpenThermTrafficRequest()
{
    Tracer tracer(F("handleHttpOpenThermTrafficRequest"));

    Html.writeHeader(F("OpenTherm traffic"), Nav);
    
    Html.writeDivStart(F("flex-container"));

    writeHtmlOpenThermDataTable(F("Thermostat requests"), thermostatRequests);
    writeHtmlOpenThermDataTable(F("Thermostat overrides"), otgwRequests);
    writeHtmlOpenThermDataTable(F("Boiler responses"), boilerResponses);
    writeHtmlOpenThermDataTable(F("Boiler overrides"), otgwResponses);

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    Html.writeHeader(F("OpenTherm log"), Nav);
    
    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((OpenThermLog.count() - 1) / OT_LOG_PAGE_SIZE) + 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    Html.writeRowStart();
    for (PGM_P header : LogHeaders)
    {
        Html.writeHeaderCell(FPSTR(header));
    }
    Html.writeRowEnd();

    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    for (int i = 0; i < (currentPage * OT_LOG_PAGE_SIZE) && otLogEntryPtr != nullptr; i++)
    {
        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
    for (int j = 0; j < OT_LOG_PAGE_SIZE && otLogEntryPtr != nullptr; j++)
    {
        Html.writeRowStart();
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
        Html.writeCell(getDecimal(otLogEntryPtr->pressure), F("%0.2f"));
        Html.writeCell(getInteger(otLogEntryPtr->boilerRelModulation));
        Html.writeRowEnd();

        otLogEntryPtr = OpenThermLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpOpenThermLogSyncRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogSyncRequest"));

    Html.writeHeader(F("FTP Sync"), Nav);

    Html.writeParagraph(
        F("Sending %d OpenTherm log entries to FTP server (%s) ..."),
        otLogEntriesToSync,
        PersistentData.ftpServer);

    Html.writePreStart();
    bool success = trySyncOpenThermLog(&HttpResponse);
    Html.writePreEnd(); 

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        otLogSyncTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());
 
    Html.writeHeading(F("CSV header"), 2);
    Html.writePreStart();
    for (PGM_P header : LogHeaders)
    {
        HttpResponse.print(FPSTR(header));
        HttpResponse.print(';');
    }
    Html.writePreEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
    destination.printf(";%0.2f", getDecimal(otLogEntryPtr->pHeatPump));
    destination.printf(";%0.2f", getDecimal(otLogEntryPtr->pressure));
    destination.printf(";%d\r\n", getInteger(otLogEntryPtr->boilerRelModulation));
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

    WebServer.send(200, ContentTypeText, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), Nav);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv(F("%s"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCommandFormRequest()
{
    Tracer tracer(F("handleHttpCommandFormRequest"));

    String cmd = WebServer.arg(F("cmd"));
    String value = WebServer.arg(F("value"));

    if (cmd.length() == 0) 
    {
        cmd = F("PR");
        value = F("A");
    }

    String tsetLowHref = F("?cmd=CS&value=");
    tsetLowHref += boilerTSet[BoilerLevel::Low];

    Html.writeHeader(F("OTGW Command"), Nav);

    Html.writeLink(F("?cmd=PR&value=A"), F("OTGW version"), ButtonClass);
    Html.writeLink(tsetLowHref, F("Set boiler level Low"), ButtonClass);
    Html.writeLink(F("?cmd=CS&value=0"), F("Stop override"), ButtonClass);

    Html.writeFormStart(F("cmd"));
    Html.writeTableStart();
    Html.writeTextBox(F("cmd"), F("Command"), cmd, 2);
    Html.writeTextBox(F("value"), F("Value"), value, 16);
    Html.writeTableEnd();
    Html.writeSubmitButton(F("Send command"));
    Html.writeFormEnd();

    Html.writeHeading(F("OTGW Response"), 2);
    Html.writePreStart();
    HttpResponse.print(otgwResponse);
    Html.writePreEnd();

    Html.writeFooter();

    otgwResponse = String();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
        if (OTGW.sendCommand(cmd, value))
            otgwResponse = OTGW.getResponse();
        else
        {
            otgwResponse = F("No valid response received. ");
            otgwResponse += OTGW.getResponse();
        }
    }

    handleHttpCommandFormRequest();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"));
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1, F("password"));
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_HEATMON_HOST, F("Heatmon host"), PersistentData.heatmonHost, sizeof(PersistentData.heatmonHost) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1, F("password"));
    Html.writeNumberBox(CFG_FTP_SYNC_ENTRIES, F("FTP Sync Entries"), PersistentData.ftpSyncEntries, 0, OT_LOG_LENGTH);
    Html.writeTextBox(CFG_WEATHER_API_KEY, F("Weather API Key"), PersistentData.weatherApiKey, 16);
    Html.writeTextBox(CFG_WEATHER_LOC, F("Weather Location"), PersistentData.weatherLocation, 16);
    Html.writeNumberBox(CFG_MAX_TSET, F("Max TSet"), PersistentData.maxTSet, 20, 90);
    Html.writeNumberBox(CFG_MIN_TSET, F("Min TSet"), PersistentData.minTSet, 20, 90);
    Html.writeNumberBox(CFG_BOILER_ON_DELAY, F("Boiler on delay (s)"), PersistentData.boilerOnDelay, 0, 7200);
    Html.writeCheckbox(CFG_PUMP_MOD, F("Pump Modulation"), PersistentData.usePumpModulation);
    Html.writeSubmitButton(F("Save"));
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
    {
        Html.writeParagraph(F("Resetting..."));
        WiFiSM.reset();
    }
    else
        Html.writeActionLink(F("reset"), F("Reset ESP"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
