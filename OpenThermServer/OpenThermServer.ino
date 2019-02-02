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
#define OTGW_STARTUP_TIME 5
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
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
bool isInitialized = false;

uint16_t otgwResets = 0;

Log eventLog(MAX_EVENT_LOG_SIZE);
Log openThermLog(OT_LOG_LENGTH);

uint16_t thermostatRequests[256]; // Thermostat request data values indexed by data ID
uint16_t boilerResponses[256]; // Boiler response data values indexed by data ID.
uint16_t otgwRequests[256]; // OTGW request data values (thermostat overrides) indexed by data ID.
uint16_t otgwResponses[256]; // OTGW response data values (boiler overrides) indexed by data ID.

uint32_t watchdogFeedTime = 0;
time_t currentTime = 0;
time_t lastOpenThermLogTime = 0;
time_t otgwInitializeTime = 0;

char cmdResponse[128];

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

    TRACE("Boot mode: %d\n", ESP.getBootMode());
    TRACE("Free heap: %d\n", ESP.getFreeHeap());

    // Read persistent data from EEPROM or initialize to defaults.
    // TODO: if (!PersistentData.readFromEEPROM())
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
    lastOpenThermLogTime = currentTime;

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

    logEvent("Initializing OTGW");

    setMaxTSet();

    // TODO: Set LED functions based on Configuration?
    // TODO: Set GPIO functions (outside temperature sensor)
    
    otgwInitializeTime = 0;
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
        // We don't feed the OTGW watchdog, so it will reset the ESP after a while.
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

    if ((otgwInitializeTime != 0) && (currentTime >= otgwInitializeTime))
        initializeOpenThermGateway();

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
    
    uint16_t lastThermostatStatus = thermostatRequests[OpenThermDataId::Status];
    if (lastThermostatStatus & OpenThermStatus::MasterCHEnable)
        otLogEntryPtr->thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    else
        otLogEntryPtr->thermostatTSet = 0; // CH disabled

    otLogEntryPtr->thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
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
            snprintf(event, sizeof(event), "Unexpected OTGW Response: %s", otgwMessage.message.c_str());
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
        int maxTSet = static_cast<int>(getDecimal(otFrame.dataValue));
        if (maxTSet != boilerTSet[BoilerLevel::High])
        {
            logEvent("Re-applying Max CH Water Setpoint because it changed (by OTGW reset?)");
            if (!setMaxTSet())
            {
                logEvent("Resetting OTGW because it does not respond.");
                OTGW.reset();
                otgwResets++;
                otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
            }
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
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
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
    HttpResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), thermostatRequests[OpenThermDataId::Status]);
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>Max Modulation %%</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation]));
    HttpResponse.printf(F("<tr><td>Max TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(thermostatRequests[OpenThermDataId::MaxTSet]));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h3>Boiler</h3>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), boilerResponses[OpenThermDataId::Status]);
    HttpResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TSet]));
    HttpResponse.printf(F("<tr><td>TWater</td><td>%0.1f</td></tr>\r\n"), getDecimal(boilerResponses[OpenThermDataId::TBoiler]));
    HttpResponse.printf(F("<tr><td>Burner starts</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerStarts]);
    HttpResponse.printf(F("<tr><td>Burner hours</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    HttpResponse.printf(F("<tr><td>DHW hours</td><td>%d</td></tr>\r\n"), boilerResponses[OpenThermDataId::BoilerDHWBurnerHours]);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p class=\"traffic\"><a href=\"/traffic\">View all OpenTherm traffic</a></p>"));

    HttpResponse.println(F("<h2>OpenTherm Gateway status</h2>"));
    HttpResponse.println(F("<table>"));
    for (int i = 0; i <= 4; i++)
        HttpResponse.printf(F("<tr><td>Error %02X</td><td>%d</td></tr>\r\n"), i, OTGW.errors[i]);
    HttpResponse.printf(F("<tr><td>OTGW Resets</td><td>%d</td></tr>\r\n"), otgwResets);
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
    
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Time</th><th>TSet</th><th>Max mod %</th><th>TWater</th></tr>"));
    char timeString[8];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(timeString, sizeof(timeString), "%H:%M", otLogEntryPtr->time);
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

    /* JSON format:
    [
        {"Time":"2019-02-02 12:30", "TSet":45.0, "MaxRelMod":100.0, "TWater":44.0},
        {"Time":"2019-02-02 12:35", "TSet":40.0, "MaxRelMod":0.0, "TWater":42.0}
    ]
    */

    HttpResponse.clear();
    HttpResponse.println("[");

    char timeString[strlen("2019-02-02 12:30") + 1];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        OpenThermLogEntry* nextOTLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());

        formatTime(timeString, sizeof(timeString), "%F %H:%M", otLogEntryPtr->time);
        HttpResponse.printf(
            F("\t{\"Time\":\"%s\", \"TSet\":%0.1f, \"MaxRelMod\":%0.1f, \"TWater\":%0.1f}%s\r\n"), 
            timeString, 
            getDecimal(otLogEntryPtr->thermostatTSet), 
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation), 
            getDecimal(otLogEntryPtr->boilerTWater),
            (nextOTLogEntryPtr == NULL) ? "" : ","
            );

        otLogEntryPtr = nextOTLogEntryPtr;
    }

    HttpResponse.println("]");

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


void handleHttpCommandFormRequest()
{
    Tracer tracer("handleHttpCommandFormRequest");

    writeHtmlHeader("Send OTGW Command", true, true);

    HttpResponse.println(F("<form action=\"/cmd\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, "cmd", "PR", "Command");
    addTextBoxRow(HttpResponse, "value", "M", "Value");
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
