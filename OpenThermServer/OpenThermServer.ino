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
#define MAX_EVENT_LOG_SIZE 100
#define WATCHDOG_INTERVAL_MS 1000
#define POLL_INTERVAL 60
#define DATA_VALUE_NONE 0xFFFF
#define OT_LOG_INTERVAL 5*60
#define OT_LOG_LENGTH 256
#define KEEP_TSET_HIGH_DURATION 60*60
#define KEEP_TSET_LOW_DURATION 10*60
#define LOW_POWER_MODULATION_THRESHOLD 25*256

typedef enum
{
    Off,
    Low,
    Medium,
    High
} BoilerLevel;

struct OpenThermLogEntry
{
    time_t time;
    uint16_t thermostatTSet;
    uint16_t thermostatMaxRelModulation;
    uint16_t boilerTSet;
    uint16_t boilerTWater;
};

struct PersistentDataClass : PersistentDataBase
{
    public:
        char HostName[16];
        int TimeZoneOffset;

        PersistentDataClass() : PersistentDataBase(sizeof(HostName) + sizeof(TimeZoneOffset)) {}
};

PersistentDataClass PersistentData;
OpenThermGateway OTGW(Serial, 14);
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer("0.europe.pool.ntp.org", 24 * 3600); // Synchronize daily
StringBuilder HttpResponse(8192); // 32KB HTTP response buffer
bool isInitialized = false;

Log eventLog(MAX_EVENT_LOG_SIZE);
Log openThermLog(OT_LOG_LENGTH);

uint16_t lastThermostatStatus;
uint16_t lastThermostatTSet;
uint16_t lastThermostatMaxRelModulation;
int lastThermostatMaxTSet;
uint16_t lastBoilerStatus;
uint16_t lastBoilerTSet;
uint16_t lastBoilerTWater;

uint16_t thermostatRequests[256]; // Thermostat request data values indexed by data ID
uint16_t boilerResponses[256]; // Boiler response data values indexed by data ID.

uint32_t watchdogFeedTime = 0;
time_t currentTime = 0;
time_t lastOpenThermLogTime = 0;

int boilerTSet[4] = {0, 40, 50, 60}; // TODO: configurable

BoilerLevel currentBoilerLevel;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime;


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
    Serial.setTimeout(500);
    //Tracer::traceTo(Serial);

    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    TRACE("Free heap: %d\n", ESP.getFreeHeap());
    TRACE("Boot mode: %d\n", ESP.getBootMode());

    // Read persistent data from EEPROM or initialize to defaults.
    //if (!PersistentData.readFromEEPROM())
    {
        TRACE("EEPROM not initialized; initializing with defaults.\n");
        strcpy(PersistentData.HostName, "OpenThermGateway");
        PersistentData.TimeZoneOffset = 0;
    }
    
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
    WebServer.on("/log-json", handleHttpOpenThermLogJsonRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
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

    lastOpenThermLogTime = currentTime;

    lastThermostatStatus = DATA_VALUE_NONE;
    lastThermostatTSet = DATA_VALUE_NONE;
    lastThermostatMaxRelModulation = DATA_VALUE_NONE;
    lastThermostatMaxTSet = 0;
    lastBoilerStatus = DATA_VALUE_NONE;
    lastBoilerTWater = DATA_VALUE_NONE;

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));

    initializeOpenThermGateway();

    /*
    setBoilerLevel(BoilerLevel::Off);
    changeBoilerLevel = BoilerLevel::Off;
    changeBoilerLevelTime = 0;
    */

    TRACE("Free heap: %d\n", ESP.getFreeHeap());

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);

    logEvent("Initialized after boot.");
    isInitialized = true;
}


void initializeOpenThermGateway()
{
    Tracer tracer("initializeOpenThermGateway");

    setMaxTSet();

    // TODO: Set LED functions based on Configuration?
    // TODO: Set GPIO functions (outside temperature sensor)
}


bool setMaxTSet()
{
    Tracer tracer("setMaxTSet");

    char maxTSet[8];
    sprintf(maxTSet, "%d", boilerTSet[BoilerLevel::High]);
    
    bool success = OTGW.sendCommand("SH", maxTSet); 
    if (!success)
        logEvent("Unable to set max CH water setpoint");

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    Tracer tracer("setBoilerLevel");

    currentBoilerLevel = level;

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
        logEvent("Unable to set boiler level");

    return success;
}


// Called repeatedly
void loop() 
{
    if (!isInitialized)
    {
        // Initialization failed. Blink LED.
        digitalWrite(LED_BUILTIN, 0);
        delay(500);
        digitalWrite(LED_BUILTIN, 1);
        delay(500);
        return;
    }
    
    WebServer.handleClient();

    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, 0);
        handleSerialData();
        digitalWrite(LED_BUILTIN, 1);
    }

    if (millis() >= watchdogFeedTime)
    {
        OTGW.feedWatchdog();
        watchdogFeedTime = millis() + WATCHDOG_INTERVAL_MS;
    }

    currentTime = TimeServer.getCurrentTime();

    // Log OpenTherm values from Thermostat and Boiler
    if (currentTime >= lastOpenThermLogTime + OT_LOG_INTERVAL)
    {
        lastOpenThermLogTime = currentTime;
        logOpenThermValues();
        
        TRACE("Free heap: %d\n", ESP.getFreeHeap());
    }

    // Scheduled Boiler TSet change
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        setBoilerLevel(changeBoilerLevel);
        changeBoilerLevelTime = 0;
    }
    
    delay(10);
}


void logOpenThermValues()
{
    Tracer tracer("logOpenThermValues");

    OpenThermLogEntry* otLogEntryPtr = new OpenThermLogEntry();
    otLogEntryPtr->time = currentTime;
    
    if (lastThermostatStatus & OpenThermStatus::MasterCHEnable)
        otLogEntryPtr->thermostatTSet = lastThermostatTSet;
    else
        otLogEntryPtr->thermostatTSet = 0; // CH disabled

    otLogEntryPtr->thermostatMaxRelModulation = lastThermostatMaxRelModulation;
    otLogEntryPtr->boilerTSet = lastBoilerTSet;
    otLogEntryPtr->boilerTWater = lastBoilerTWater;

    openThermLog.add(otLogEntryPtr);

    TRACE("%d OpenTherm log entries.\n", openThermLog.count());
}


void handleSerialData()
{
    Tracer tracer("handleSerialData");

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
            char event[64];
            snprintf(event, sizeof(event), "Error from OpenTherm Gateway: %s", otgwMessage.message.c_str());
            logEvent(event);
    }
}


float getDecimal(uint16_t dataValue)
{
    if (dataValue == DATA_VALUE_NONE)
        return 0.0;
    else
        return float(static_cast<int16_t>(dataValue)) / 256;
}


void handleThermostatRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleThermostatRequest");
    
    thermostatRequests[otFrame.dataId] = otFrame.dataValue;

    if (otFrame.dataId == OpenThermDataId::Status)
    {
        // Don't override thermostat's TSet; it seems its doing fine as long as max CH Water TSet is properly set.
        /*
        bool masterCHEnable = otFrame.dataValue & OpenThermStatus::MasterCHEnable;
        bool lastMasterCHEnable = lastThermostatStatus & OpenThermStatus::MasterCHEnable;
        if (masterCHEnable && !lastMasterCHEnable)
        {
            // CH switched on
            if (currentBoilerLevel == BoilerLevel::Off)
            {
                if (lastThermostatMaxRelModulation > LOW_POWER_MODULATION_THRESHOLD)
                {
                    setBoilerLevel(BoilerLevel::High);
                    changeBoilerLevel = BoilerLevel::Medium;
                    changeBoilerLevelTime = currentTime + KEEP_TSET_HIGH_DURATION;
                }
                else
                    setBoilerLevel(BoilerLevel::Low); // TODO: Medium ?
            }
            else if (currentBoilerLevel == BoilerLevel::Low)
            {
                changeBoilerLevel = BoilerLevel::Medium;
                changeBoilerLevelTime = currentTime + KEEP_TSET_LOW_DURATION;
            }
        }
        else if (!masterCHEnable && lastMasterCHEnable)
        {
            // CH switched off
            setBoilerLevel(BoilerLevel::Low);
            changeBoilerLevel = BoilerLevel::Off;
            changeBoilerLevelTime = currentTime + KEEP_TSET_LOW_DURATION;
        }
        */
        lastThermostatStatus = otFrame.dataValue;
    }
    else if (otFrame.dataId == OpenThermDataId::TSet)
    {
        lastThermostatTSet = otFrame.dataValue;
    }
    else if (otFrame.dataId == OpenThermDataId::MaxRelModulation)
    {
        lastThermostatMaxRelModulation = otFrame.dataValue;
        // Don't override thermostat's TSet; it seems its doing fine as long as max CH Water TSet is properly set.
        /*
        if (lastThermostatMaxRelModulation > LOW_POWER_MODULATION_THRESHOLD)
        {
            if (currentBoilerLevel == BoilerLevel::Low)
                setBoilerLevel(BoilerLevel::Medium);
        }
        else
        {
            if (currentBoilerLevel > BoilerLevel::Low)
                setBoilerLevel(BoilerLevel::Low);
        }
        */
    }
}


void handleBoilerResponse(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleBoilerResponse");

    if (otFrame.msgType == OpenThermMsgType::UnknownDataId)
        return;
    
    boilerResponses[otFrame.dataId] = otFrame.dataValue;

    switch (otFrame.dataId)
    {
        case OpenThermDataId::Status:
            lastBoilerStatus = otFrame.dataValue;
            break;
      
        case OpenThermDataId::TSet:
            lastBoilerTSet = otFrame.dataValue;
            break;
          
        case OpenThermDataId::TBoiler:
            lastBoilerTWater = otFrame.dataValue;
            break;

        default:
          // Nothing to do
          break;
    }
}


void handleBoilerRequest(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleBoilerRequest");

    // Modified request from OTGW to boiler (e.g. TSet override)
}


void handleThermostatResponse(OpenThermGatewayMessage otFrame)
{
    Tracer tracer("handleThermostatResponse");

    // Modified response from OTGW to thermostat (e.g. TOutside override)
    switch (otFrame.dataId)
    {
        case OpenThermDataId::MaxTSet:
            lastThermostatMaxTSet = static_cast<int>(getDecimal(otFrame.dataValue));
            if (lastThermostatMaxTSet != boilerTSet[BoilerLevel::High])
            {
                logEvent("Re-applying Max CH Water Setpoint because it changed (by OTGW reset?)");
                if (!setMaxTSet())
                {
                    logEvent("Resetting OpenTherm Gateway because it does not respond.");
                    OTGW.reset();
                }
            }
            break;

        default:
          // Nothing to do
          break;
    }
}


void writeHtmlHeader(const char* title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.HostName, title);
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"196x196\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"196x196\" href=\"%s\">\r\n"), ICON, ICON);
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
    HttpResponse.println(F("</head>"));
    
    HttpResponse.println(F("<body>"));
    if (includeHomePageLink)
      HttpResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
    if (includeHeading)
      HttpResponse.printf(F("<h1>%s</h1>"), title);
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
    HttpResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), lastThermostatStatus);
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastThermostatTSet));
    HttpResponse.printf(F("<tr><td>Max Modulation %</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastThermostatMaxRelModulation));
    HttpResponse.printf(F("<tr><td>Max TSet</td><td>%d</td></tr>\r\n"), lastThermostatMaxTSet);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h3>Boiler</h3>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), lastBoilerStatus);
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastBoilerTSet));
    HttpResponse.printf(F("<tr><td>TWater</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastBoilerTWater));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p class=\"traffic\"><a href=\"/traffic\">View all OpenTherm traffic</a></p>"));

    /*
    char timeString[8];
    if (changeBoilerLevelTime == 0)
        sprintf(timeString, "Not set");
    else
        formatTime(timeString, sizeof(timeString), "%H:%M", changeBoilerLevelTime);

    HttpResponse.println(F("<h2>Boiler TSet override</h2>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>currentBoilerLevelt level</td><td>%d</td></tr>\r\n"), currentBoilerLevel);
    HttpResponse.printf(F("<tr><td>Change to level</td><td>%d</td></tr>\r\n"), changeBoilerLevel);
    HttpResponse.printf(F("<tr><td>Change at time</td><td>%s</td></tr>\r\n"), timeString);
    formatTime(timeString, sizeof(timeString), "%H:%M", currentTime);
    HttpResponse.printf(F("<tr><td>currentBoilerLevelt time</td><td>%s</td></tr>\r\n"), timeString);
    HttpResponse.println(F("</table>"));
    */

    HttpResponse.println(F("<h2>OpenTherm Gateway status</h2>"));
    HttpResponse.println(F("<table>"));
    for (int i = 1; i <= 4; i++)
        HttpResponse.printf(F("<tr><td>Error %02X</td><td>%d</td></tr>\r\n"), i, OTGW.errors[i]);
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), eventLog.count());
    HttpResponse.printf(F("<p class=\"log\"><a href=\"/log\">%d OpenTherm log entries.</a></p>\r\n"), openThermLog.count());

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpOpenThermTrafficRequest()
{
    Tracer tracer("handleHttpOpenThermTrafficRequest");

    writeHtmlHeader("OpenTherm traffic", true, true);
    
    HttpResponse.println(F("<h2>Thermostat requests</h2>"));
    HttpResponse.println(F("<table>"));
    for (int i = 0; i < 256; i++)
    {
        uint16_t thermostatRequest = thermostatRequests[i];
        if (thermostatRequest != DATA_VALUE_NONE)
            HttpResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, thermostatRequest, getDecimal(thermostatRequest));
    }
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h2>Boiler responses</h2>"));
    HttpResponse.println(F("<table>"));
    for (int i = 0; i < 256; i++)
    {
      uint16_t boilerResponse = boilerResponses[i];
      if (boilerResponse != DATA_VALUE_NONE)
          HttpResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, boilerResponse, getDecimal(boilerResponse));
    }
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer("handleHttpOpenThermLogRequest");

    writeHtmlHeader("OpenTherm log", true, true);
    
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><td>Time</td><td>TSet</td><td>Max mod %</td><td>TWater</td></tr>"));
    char timeString[8];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(timeString, sizeof(timeString), "%F %H:%M", otLogEntryPtr->time);
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td></tr>\r\n"), 
            timeString, 
            getDecimal(otLogEntryPtr->thermostatTSet), 
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation), 
            getDecimal(otLogEntryPtr->boilerTWater)
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
    }
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpOpenThermLogJsonRequest()
{
    Tracer tracer("handleHttpOpenThermLogJsonRequest");

    // TODO: write a JSON response
    HttpResponse.clear();

    for (int i = 0; i < 256; i++)
    {
        uint16_t thermostatRequest = thermostatRequests[i];
        if (thermostatRequest != DATA_VALUE_NONE)
            HttpResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, thermostatRequest, getDecimal(thermostatRequest));
    }

    for (int i = 0; i < 256; i++)
    {
      uint16_t boilerResponse = boilerResponses[i];
      if (boilerResponse != DATA_VALUE_NONE)
          HttpResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, boilerResponse, getDecimal(boilerResponse));
    }

    char timeString[8];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(timeString, sizeof(timeString), "%F %H:%M", otLogEntryPtr->time);
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td<td>%0.1f</td></tr>\r\n"), 
            timeString, 
            getDecimal(otLogEntryPtr->thermostatTSet), 
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation), 
            getDecimal(otLogEntryPtr->boilerTWater)
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
    }

    WebServer.send(200, "application/json", HttpResponse);
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

    HttpResponse.println(F("<div><a href=\"/events/clear\">Clear event log</a></div>"));

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


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");

    char tzOffsetString[4];
    sprintf(tzOffsetString, "%d", PersistentData.TimeZoneOffset);

    writeHtmlHeader("Configuration", true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, "hostName", PersistentData.HostName, "Host name");
    addTextBoxRow(HttpResponse, "tzOffset", tzOffsetString, "Timezone offset");
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    strcpy(PersistentData.HostName, WebServer.arg("hostName").c_str()); 
    String tzOffsetString = WebServer.arg("tzOffset");

    TRACE("hostName: %s\n", PersistentData.HostName);
    TRACE("tzOffset: %s\n", tzOffsetString.c_str());
    
    sscanf(tzOffsetString.c_str(), "%d", &PersistentData.TimeZoneOffset);

    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();

    // TODO: restart (?)
}


void handleHttpNotFound()
{
    TRACE("Unexpected HTTP request: %s\n", WebServer.uri().c_str());
    WebServer.send(404, "text/plain", "Unexpected request.");
}
