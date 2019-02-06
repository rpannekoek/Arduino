#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <WiFiNTP.h>
#include <Tracer.h>
#include <PrintHex.h>
#include <StringBuilder.h>
#include <OTGW.h>
#include <PersistentDataBase.h>
#include <Log.h>
#include "WiFiCredentials.private.h"

#define ICON "/apple-touch-icon.png"
#define WATCHDOG_INTERVAL_MS 1000
#define OTGW_STARTUP_TIME 5
#define OTGW_TIMEOUT 60
#define HTTP_POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define EVENT_LOG_LENGTH 100
#define OT_LOG_LENGTH 240
#define KEEP_TSET_LOW_DURATION 10*60

const char* _boilerLevelNames[5] = {"Off", "Low", "Medium", "High", "Thermostat"};

typedef enum
{
    Off,
    Low,
    Medium,
    High,
    Thermostat
} BoilerLevel;

struct OpenThermLogEntry
{
    time_t time;
    uint16_t thermostatTSet;
    uint16_t thermostatMaxRelModulation;
    uint16_t boilerStatus;
    uint16_t boilerTSet;
    uint16_t boilerTWater;
};

struct PersistentDataClass : PersistentDataBase
{
    char HostName[20];
    int8_t TimeZoneOffset; // hours
    uint16_t OpenThermLogInterval; // seconds

    PersistentDataClass() 
        : PersistentDataBase(sizeof(HostName) + sizeof(TimeZoneOffset) + sizeof(OpenThermLogInterval)) {}

    void initialize()
    {
        strcpy(HostName, "OpenThermGateway");
        TimeZoneOffset = 1;
        OpenThermLogInterval = 60;
    }
};

PersistentDataClass PersistentData;
OpenThermGateway OTGW(Serial, 14);
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer("0.europe.pool.ntp.org", 24 * 3600); // Synchronize daily
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
bool isInitialized = false;

Log eventLog(EVENT_LOG_LENGTH);
Log openThermLog(OT_LOG_LENGTH);

uint16_t thermostatRequests[256]; // Thermostat request data values indexed by data ID
uint16_t boilerResponses[256]; // Boiler response data values indexed by data ID.
uint16_t otgwRequests[256]; // OTGW request data values (thermostat overrides) indexed by data ID.
uint16_t otgwResponses[256]; // OTGW response data values (boiler overrides) indexed by data ID.

uint32_t watchdogFeedTime = 0;
time_t currentTime = 0;
time_t otLogTime = 0;
time_t otgwInitializeTime = 0;
time_t otgwTimeout = 0;

char cmdResponse[128];

int boilerTSet[5] = {0, 40, 50, 60, 0}; // TODO: configurable

BoilerLevel currentBoilerLevel = BoilerLevel::Thermostat;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime = 0;
time_t boilerOverrideStartTime = 0;
uint32_t totalOverrideDuration = 0;


int formatTime(char* output, size_t output_size, const char* format, time_t time)
{
    time += PersistentData.TimeZoneOffset * 3600;
    return strftime(output, output_size, format, gmtime(&time));
}


void logEvent(const char* msg)
{
    Tracer tracer("logEvent", msg);

    size_t timestamp_size = strlen("2019-01-30 12:23:34  : ");

    char* event = new char[timestamp_size + strlen(msg) + 1];
    formatTime(event, timestamp_size, "%F %H:%M:%S : ", TimeServer.getCurrentTime());
    strcat(event, msg);

    eventLog.add(event);

    TRACE("%d event log entries\n", eventLog.count());
}


// Boot code
void setup() 
{
    Serial.begin(9600);
    Serial.setTimeout(1000);
    //Tracer::traceTo(Serial);

    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    TRACE("Boot mode: %d\n", ESP.getBootMode());
    TRACE("Free heap: %d\n", ESP.getFreeHeap());

    // Read persistent data from EEPROM or initialize to defaults.
    if (!PersistentData.readFromEEPROM())
    {
        TRACE("EEPROM not initialized; initializing with defaults.\n");
        PersistentData.initialize();
        PersistentData.printData();
    }
    if (PersistentData.OpenThermLogInterval < 5)
        PersistentData.OpenThermLogInterval = 5;

    // Connect to WiFi network
    TRACE("Connecting to WiFi network '%s' ", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.hostname(PersistentData.HostName);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int i = 0;
    while (WiFi.status() == WL_IDLE_STATUS || WiFi.status() == WL_DISCONNECTED)
    {
        TRACE(".");
        delay(500);
        if (i++ == 60)
        {
            TRACE("\nTimeout connecting WiFi.\n");
            return;
        }
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        TRACE("\nError connecting WiFi. Status: %d\n", WiFi.status());
        return;
    }
    TRACE("\nWiFi connected. IP address: %s\n", WiFi.localIP().toString().c_str());

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log", handleHttpOpenThermLogRequest);
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
    WebServer.begin();
    TRACE("Web Server started\n");
    
    currentTime = TimeServer.getCurrentTime();
    if (currentTime < 1000) 
        logEvent("Unable to obtain time from NTP server.");

    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;
    otLogTime = otgwInitializeTime + PersistentData.OpenThermLogInterval;

    cmdResponse[0] = 0;

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));
    memset(otgwRequests, 0xFF, sizeof(otgwRequests));
    memset(otgwResponses, 0xFF, sizeof(otgwResponses));

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);

    logEvent("Initialized after boot.");
    isInitialized = true;

    TRACE("Free heap: %d\n", ESP.getFreeHeap());
}


void initializeOpenThermGateway()
{
    Tracer tracer("initializeOpenThermGateway");

    bool success = setMaxTSet();

    // TODO: Set LED functions based on Configuration?
    // TODO: Set GPIO functions (outside temperature sensor)

    if (success)
        logEvent("OTGW initialized.");
}


void resetOpenThermGateway()
{
    Tracer tracer("resetOpenThermGateway");

    OTGW.reset();
    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;

    logEvent("OTGW reset");
}


bool setMaxTSet()
{
    Tracer tracer("setMaxTSet");

    char maxTSet[8];
    sprintf(maxTSet, "%d", boilerTSet[BoilerLevel::High]);
    
    bool success = OTGW.sendCommand("SH", maxTSet); 
    if (!success)
    {
        logEvent("Unable to set max CH water setpoint");
        resetOpenThermGateway();
    }

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    Tracer tracer("setBoilerLevel");

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
        char tSet[8];
        sprintf(tSet, "%d", boilerTSet[level]);
        success = OTGW.sendCommand("CS", tSet);
    }

    if (!success)
    {
        logEvent("Unable to set boiler level");
        resetOpenThermGateway();
    }

    return success;
}


// Called repeatedly
void loop() 
{
    if (!isInitialized)
    {
        // Initialization failed. Blink LED.
        // We don't feed the OTGW watchdog, so it will reset the ESP after a while.
        digitalWrite(LED_BUILTIN, 0);
        delay(500);
        digitalWrite(LED_BUILTIN, 1);
        delay(500);
        return;
    }
    
    currentTime = TimeServer.getCurrentTime();

    WebServer.handleClient();

    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, 0);
        handleSerialData();
        otgwTimeout = currentTime + OTGW_TIMEOUT;
        digitalWrite(LED_BUILTIN, 1);
    }

    if (millis() >= watchdogFeedTime)
    {
        OTGW.feedWatchdog();
        watchdogFeedTime = millis() + WATCHDOG_INTERVAL_MS;
    }

    if (currentTime >= otgwTimeout)
    {
        logEvent("OTGW Timeout");
        resetOpenThermGateway();
    }

    if ((otgwInitializeTime != 0) && (currentTime >= otgwInitializeTime))
    {
        otgwInitializeTime = 0;
        initializeOpenThermGateway();
    }

    // Log OpenTherm values from Thermostat and Boiler
    if (currentTime >= otLogTime)
    {
        logOpenThermValues();
        otLogTime = currentTime + PersistentData.OpenThermLogInterval;
        
        TRACE("Free heap: %d\n", ESP.getFreeHeap());
    }

    // Scheduled Boiler TSet change
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        changeBoilerLevelTime = 0;
        setBoilerLevel(changeBoilerLevel);
    }

    delay(10);
}


void logOpenThermValues()
{
    Tracer tracer("logOpenThermValues");

    OpenThermLogEntry* otLogEntryPtr = new OpenThermLogEntry();
    otLogEntryPtr->time = currentTime;
    
    uint16_t lastThermostatStatus = thermostatRequests[OpenThermDataId::Status];
    if (lastThermostatStatus & OpenThermStatus::MasterCHEnable)
        otLogEntryPtr->thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    else
        otLogEntryPtr->thermostatTSet = 0; // CH disabled

    otLogEntryPtr->thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
    otLogEntryPtr->boilerStatus = boilerResponses[OpenThermDataId::Status];
    otLogEntryPtr->boilerTSet = boilerResponses[OpenThermDataId::TSet];
    otLogEntryPtr->boilerTWater = boilerResponses[OpenThermDataId::TBoiler];

    openThermLog.add(otLogEntryPtr);

    TRACE("%d OpenTherm log entries.\n", openThermLog.count());
}


void handleSerialData()
{
    Tracer tracer("handleSerialData");
    char event[64];

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

        case OpenThermGatewayDirection::Error:
            snprintf(event, sizeof(event), "OTGW Error: %s", otgwMessage.message.c_str());
            logEvent(event);
            break;

        case OpenThermGatewayDirection::Unexpected:
            if (otgwInitializeTime == 0)
            {
                snprintf(event, sizeof(event), "Unexpected OTGW Message: %s", otgwMessage.message.c_str());
                logEvent(event);
            }
            else
                logEvent(otgwMessage.message.c_str());
    }
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
        return static_cast<int8_t>(dataValue >> 8);
}


void handleThermostatRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleThermostatRequest");
    
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
    Tracer tracer("handleBoilerResponse");

    if (otFrame.msgType == OpenThermMsgType::UnknownDataId)
        return;
    
    boilerResponses[otFrame.dataId] = otFrame.dataValue;
}


void handleBoilerRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleBoilerRequest");

    // Modified request from OTGW to boiler (e.g. TSet override)
    otgwRequests[otFrame.dataId] = otFrame.dataValue;
}


void handleThermostatResponse(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleThermostatResponse");

    // Modified response from OTGW to thermostat (e.g. TOutside override)
    otgwResponses[otFrame.dataId] = otFrame.dataValue;

    if (otFrame.dataId == OpenThermDataId::MaxTSet)
    {
        int8_t maxTSet = getInteger(otFrame.dataValue);
        if (maxTSet != boilerTSet[BoilerLevel::High])
        {
            logEvent("Max CH Water Setpoint is changed (by OTGW reset?). Re-initializing OTGW.");
            otgwInitializeTime = currentTime;
        }
    }
}


void writeHtmlHeader(const char* title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.HostName, title);
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"196x196\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"196x196\" href=\"%s\">\r\n"), ICON, ICON);
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , HTTP_POLL_INTERVAL);
    HttpResponse.println(F("</head>"));
    
    HttpResponse.println(F("<body>"));
    if (includeHomePageLink)
        HttpResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
    if (includeHeading)
        HttpResponse.printf(F("<h1>%s</h1>\r\n"), title);
}


void writeHtmlFooter()
{
    HttpResponse.println(F("</body>"));
    HttpResponse.println(F("</html>"));
}


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");
    
    writeHtmlHeader("Home", false, false);

    HttpResponse.println(F("<h2>Last OpenTherm values</h2>"));

    HttpResponse.println(F("<h3>Thermostat</h3>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), OTGW.getMasterStatus(thermostatRequests[OpenThermDataId::Status]));
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>Max Modulation %%</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation]));
    HttpResponse.printf(F("<tr><td>Max TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxTSet]));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h3>Boiler</h3>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), OTGW.getSlaveStatus(boilerResponses[OpenThermDataId::Status]));
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>TWater</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TBoiler]));
    HttpResponse.printf(F("<tr><td>Fault flags</td><td>%s</td></tr>\r\n"), OTGW.getFaultFlags(boilerResponses[OpenThermDataId::SlaveFault]));
    HttpResponse.printf(F("<tr><td>Burner starts</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerStarts]);
    HttpResponse.printf(F("<tr><td>Burner hours</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    HttpResponse.printf(F("<tr><td>DHW hours</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerDHWBurnerHours]);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p class=\"traffic\"><a href=\"/traffic\">View all OpenTherm traffic</a></p>"));

    HttpResponse.println(F("<h2>Boiler override</h2>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Current level</td><td>%s</td></tr>\r\n"), _boilerLevelNames[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        HttpResponse.printf(F("<tr><td>Change to</td><td>%s</td></tr>\r\n"), _boilerLevelNames[changeBoilerLevel]);
        HttpResponse.printf(F("<tr><td>Change in</td><td>%d s</td></tr>\r\n"), changeBoilerLevelTime - currentTime);
    }
    HttpResponse.printf(F("<tr><td>Override duration</td><td>%0.1f h</td></tr>\r\n"), float(totalOverrideDuration) / 3600);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h2>OpenTherm Gateway status</h2>"));
    HttpResponse.println(F("<table>"));
    for (int i = 0; i <= 4; i++)
        HttpResponse.printf(F("<tr><td>Error %02X</td><td>%d</td></tr>\r\n"), i, OTGW.errors[i]);
    HttpResponse.printf(F("<tr><td>OTGW Resets</td><td>%d</td></tr>\r\n"), OTGW.resets);
    HttpResponse.printf(F("<tr><td>ESP Free Heap</td><td>%d</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>ESP Reset</td><td>%s</td></tr>\r\n"), ESP.getResetReason().c_str());
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), eventLog.count());
    HttpResponse.printf(F("<p class=\"log\"><a href=\"/log\">%d OpenTherm log entries.</a></p>\r\n"), openThermLog.count());
    HttpResponse.println(F("<p class=\"cmd\"><a href=\"/cmd\">Send command to OpenTherm Gateway</a></p>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void writeHtmlOpenThermDataTable(const char* title, uint16_t* otDataTable)
{
    HttpResponse.printf(F("<h2>%s</h2>\r\n"), title);
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
    Tracer tracer("handleHttpOpenThermTrafficRequest");

    writeHtmlHeader("OpenTherm traffic", true, true);
    
    writeHtmlOpenThermDataTable("Thermostat requests", thermostatRequests);
    writeHtmlOpenThermDataTable("Boiler responses", boilerResponses);
    writeHtmlOpenThermDataTable("OTGW requests (thermostat overrides)", otgwRequests);
    writeHtmlOpenThermDataTable("OTGW responses (boiler overrides)", otgwResponses);

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer("handleHttpOpenThermLogRequest");

    writeHtmlHeader("OpenTherm log", true, true);
    
    HttpResponse.println(F("<p class=\"log-csv\"><a href=\"log-csv\">Get log in CSV format</a></p>"));

    // If the OT log contains many entries, we render every other so the max HTTP response size is not exceeded.
    bool skipEvenEntries = openThermLog.count() > 100;

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Time</th><th>TSet(t)</th><th>Max mod %</th><th>TSet(b)</th><th>TWater</th><th>Status</th></tr>"));
    char timeString[8];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(timeString, sizeof(timeString), "%H:%M", otLogEntryPtr->time);

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%s</td></tr>\r\n"),
            timeString, 
            getDecimal(otLogEntryPtr->thermostatTSet),
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation),
            getDecimal(otLogEntryPtr->boilerTSet),
            getDecimal(otLogEntryPtr->boilerTWater),
            OTGW.getSlaveStatus(otLogEntryPtr->boilerStatus)
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
        if (skipEvenEntries && (otLogEntryPtr != NULL))
            otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
    }
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpOpenThermLogCsvRequest()
{
    Tracer tracer("handleHttpOpenThermLogCsvRequest");

    /* CSV format:
    "Time","TSet(t)","Max Modulation %","TSet(b)","TWater","CH","DHW"
    "2019-02-02 12:30",45,100,45,44,5,0
    "2019-02-02 12:35",50,100,48,65,0,5
    */

    HttpResponse.clear();
    HttpResponse.println("\"Time\",\"TSet(t)\",\"Max Modulation %\",\"TSet(b)\",\"TWater\",\"CH\",\"DHW\"");

    char timeString[strlen("2019-02-02 12:30") + 1];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        int statusCH = 0;
        int statusDHW = 0;
        if (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveFlame)
        {
            statusCH = (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveCHMode) ? 5 : 0;
            statusDHW = (otLogEntryPtr->boilerStatus & OpenThermStatus::SlaveDHWMode) ? 5 : 0;
        }

        formatTime(timeString, sizeof(timeString), "%F %H:%M", otLogEntryPtr->time);
        HttpResponse.printf(
            F("\"%s\",%d,%d,%d,%d,%d,%d\r\n"), 
            timeString, 
            getInteger(otLogEntryPtr->thermostatTSet),
            getInteger(otLogEntryPtr->thermostatMaxRelModulation),
            getInteger(otLogEntryPtr->boilerTSet),
            getInteger(otLogEntryPtr->boilerTWater),
            statusCH,
            statusDHW
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
    }

    WebServer.send(200, "text/plain", HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");

    writeHtmlHeader("Event log", true, true);

    char* event = static_cast<char*>(eventLog.getFirstEntry());
    while (event != NULL)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = static_cast<char*>(eventLog.getNextEntry());
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpEventLogClearRequest()
{
    Tracer tracer("handleHttpEventLogClearRequest");

    eventLog.clear();
    logEvent("Event log cleared.");

    handleHttpEventLogRequest();
}


void addTextBoxRow(StringBuilder& output, const char* name, const char* value, const char* label)
{
    output.printf(F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\"></td></tr>\r\n"), name, label, name, value);
}


void handleHttpCommandFormRequest()
{
    Tracer tracer("handleHttpCommandFormRequest");

    writeHtmlHeader("Send OTGW Command", true, true);

    HttpResponse.println(F("<form action=\"/cmd\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, "cmd", "PR", "Command");
    addTextBoxRow(HttpResponse, "value", "A", "Value");
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.println(F("<h2>OTGW Response</h2>"));
    HttpResponse.printf(F("<div class=\"response\"><pre>%s</pre></div>"), cmdResponse);

    writeHtmlFooter();

    cmdResponse[0] = 0;

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpCommandFormPost()
{
    Tracer tracer("handleHttpCommandFormPost");

    String cmd = WebServer.arg("cmd");
    String value = WebServer.arg("value");

    TRACE("cmd: '%s'\nvalue: '%s'\n", cmd.c_str(), value.c_str());

    if (cmd.length() != 2)
        snprintf(cmdResponse, sizeof(cmdResponse), "Invalid command. Must be 2 characters.");
    else
    {
        bool success = OTGW.sendCommand(cmd.c_str(), value.c_str(), cmdResponse, sizeof(cmdResponse));
        if (!success)
            snprintf(cmdResponse, sizeof(cmdResponse), "No valid response received from OTGW.");
    }

    handleHttpCommandFormRequest();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");

    char tzOffsetString[4];
    sprintf(tzOffsetString, "%d", PersistentData.TimeZoneOffset);

    char otLogIntervalString[4];
    sprintf(otLogIntervalString, "%u", PersistentData.OpenThermLogInterval);

    writeHtmlHeader("Configuration", true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, "hostName", PersistentData.HostName, "Host name");
    addTextBoxRow(HttpResponse, "tzOffset", tzOffsetString, "Timezone offset");
    addTextBoxRow(HttpResponse, "otLogInterval", otLogIntervalString, "OT Log Interval");
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.printf(
        F("<div>OpenTherm log length: %d * %d s = %0.1f hours</div>"), 
        OT_LOG_LENGTH, 
        PersistentData.OpenThermLogInterval,
        float(OT_LOG_LENGTH * PersistentData.OpenThermLogInterval) / 3600
        );

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    strcpy(PersistentData.HostName, WebServer.arg("hostName").c_str()); 
    String tzOffsetString = WebServer.arg("tzOffset");
    String otLogIntervalString = WebServer.arg("otLogInterval");

    TRACE("hostName: %s\n", PersistentData.HostName);
    TRACE("tzOffset: %s\n", tzOffsetString.c_str());
    TRACE("otLogInterval: %s\n", otLogIntervalString.c_str());
    
    int tzOffset;
    sscanf(tzOffsetString.c_str(), "%d", &tzOffset);
    if (tzOffset < -12)
        tzOffset = -12;
    if (tzOffset> 14)
        tzOffset = 14;
    PersistentData.TimeZoneOffset = static_cast<int8_t>(tzOffset);

    int otLogInterval;
    sscanf(otLogIntervalString.c_str(), "%d", &otLogInterval);
    if (otLogInterval < 5)
        otLogInterval = 5;
    if (otLogInterval > 15*60)
        otLogInterval = 15*60;
    PersistentData.OpenThermLogInterval = static_cast<uint16_t>(otLogInterval);

    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();

    delay(1000);
    ESP.reset();
}


void handleHttpNotFound()
{
    TRACE("Unexpected HTTP request: %s\n", WebServer.uri().c_str());
    WebServer.send(404, "text/plain", "Unexpected request.");
}
