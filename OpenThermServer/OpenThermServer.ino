#define TRACE_TO_SERIAL

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
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
OpenThermGateway OTGW(Serial);
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer("0.europe.pool.ntp.org", 24 * 3600); // Synchronize daily
StringBuilder HtmlResponse(16384); // 16KB HTML response buffer
bool isInitialized = false;
Log eventLog(MAX_EVENT_LOG_SIZE);
Log openThermLog(OT_LOG_LENGTH);

uint16_t lastThermostatStatus;
uint16_t lastThermostatTSet;
uint16_t lastThermostatMaxRelModulation;
uint16_t lastBoilerStatus;
uint16_t lastBoilerTSet;
uint16_t lastBoilerTWater;

uint16_t thermostatRequests[256]; // Thermostat request data values indexed by data ID
uint16_t boilerResponses[256]; // Boiler response data values indexed by data ID.

time_t lastOpenThermLogTime = 0;

int boilerTSet[4] = {0, 40, 50, 60}; // TODO: configurable

BoilerLevel currentBoilerLevel;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime;
time_t currentTime;


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
}


void setBoilerLevel(BoilerLevel level)
{
    Tracer tracer("setBoilerLevel");

    currentBoilerLevel = level;

    if (level == BoilerLevel::Off)
        OTGW.sendCommand("CH", "0");
    else
    {
        char tSet[8];
        sprintf(tSet, "%d", boilerTSet[level]);
        OTGW.sendCommand("CS", tSet);
    }
}


// Boot code
void setup() 
{
    Serial.begin(9600);
    Serial.setTimeout(500);

    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    // Read persistent data from EEPROM or initialize to defaults.
    if (!PersistentData.readFromEEPROM())
    {
        TRACE("EEPROM not initialized; initializing with defaults.");
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
            TRACE("\nTimeout connecting WiFi.");
            return;
        }
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        TRACE("\nError connecting WiFi. Status: %d\n", WiFi.status());
        return;
    }
    TRACE("\nWiFi connected. IP address: ");
    TRACE("%s\n", String(WiFi.localIP()).c_str());

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/log", handleHttpOpenThermLogRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    WebServer.begin();
    TRACE("Web Server started");
    
    setBoilerLevel(BoilerLevel::Off);
    changeBoilerLevel = BoilerLevel::Off;
    changeBoilerLevelTime = 0;

    char maxCHWaterSetpoint[8];
    sprintf(maxCHWaterSetpoint, "%d", boilerTSet[BoilerLevel::High]);
    OTGW.sendCommand("SH", maxCHWaterSetpoint);

    // TODO: Set LED functions based on Configuration?

    currentTime = TimeServer.getCurrentTime();
    if (currentTime < 1000) 
        logEvent("Unable to obtain time from NTP server.");

    lastOpenThermLogTime = currentTime;

    lastThermostatStatus = DATA_VALUE_NONE;
    lastThermostatTSet = DATA_VALUE_NONE;
    lastThermostatMaxRelModulation = DATA_VALUE_NONE;
    lastBoilerStatus = DATA_VALUE_NONE;
    lastBoilerTWater = DATA_VALUE_NONE;

    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);

    logEvent("Initialized after boot.");
    isInitialized = true;
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

    currentTime = TimeServer.getCurrentTime();
    
    // Log OpenTherm values from Thermostat and Boiler
    if (currentTime >= lastOpenThermLogTime + OT_LOG_INTERVAL)
    {
        lastOpenThermLogTime = currentTime;

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
    }

    // Scheduled Boiler TSet change
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        setBoilerLevel(changeBoilerLevel);
        changeBoilerLevelTime = 0;
    }
    
    delay(10);
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
        lastThermostatStatus = otFrame.dataValue;
    }
    else if (otFrame.dataId == OpenThermDataId::TSet)
    {
        lastThermostatTSet = otFrame.dataValue;
    }
    else if (otFrame.dataId == OpenThermDataId::MaxRelModulation)
    {
        lastThermostatMaxRelModulation = otFrame.dataValue;
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
}


void writeHtmlHeader(const char* title, bool isHomePage, bool includeHomePageLink, bool includeHeading)
{
    HtmlResponse.clear();
    HtmlResponse.println(F("<html>"));
    
    HtmlResponse.println(F("<head>"));
    HtmlResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HtmlResponse.printf(F("<title>%s - %s</title>\r\n</head>\r\n"), PersistentData.HostName, title);
    if (isHomePage)
    {
        HtmlResponse.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), ICON, ICON);
        HtmlResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
    }
    HtmlResponse.println(F("</head>"));
    
    HtmlResponse.println(F("<body>"));
    if (includeHomePageLink)
      HtmlResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
    if (includeHeading)
      HtmlResponse.printf(F("<h1>%s</h1>"), title);
}


void writeHtmlFooter()
{
    HtmlResponse.println(F("</body>"));
    HtmlResponse.println(F("</html>"));
}


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");
    
    writeHtmlHeader("Home", true, false, false);

    HtmlResponse.println(F("<h2>Last OpenTherm values</h2>"));

    HtmlResponse.println(F("<h3>Thermostat</h3>"));
    HtmlResponse.println(F("<table>"));
    HtmlResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), lastThermostatStatus);
    HtmlResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastThermostatTSet));
    HtmlResponse.printf(F("<tr><td>Max Modulation %</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastThermostatMaxRelModulation));
    HtmlResponse.println(F("</table>"));

    HtmlResponse.println(F("<h3>Boiler</h3>"));
    HtmlResponse.println(F("<table>"));
    HtmlResponse.printf(F("<tr><td>Status</td><td>0x%04X</td></tr>\r\n"), lastBoilerStatus);
    HtmlResponse.printf(F("<tr><td>TSet</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastBoilerTSet));
    HtmlResponse.printf(F("<tr><td>TWater</td><td>%0.1f</td></tr>\r\n"), getDecimal(lastBoilerTWater));
    HtmlResponse.println(F("</table>"));

    char timeString[8];
    if (changeBoilerLevelTime == 0)
        sprintf(timeString, "Not set");
    else
        formatTime(timeString, sizeof(timeString), "%H:%M", changeBoilerLevelTime);

    HtmlResponse.println(F("<h2>Boiler TSet override</h2>"));
    HtmlResponse.println(F("<table>"));
    HtmlResponse.printf(F("<tr><td>currentBoilerLevelt level</td><td>%d</td></tr>\r\n"), currentBoilerLevel);
    HtmlResponse.printf(F("<tr><td>Change to level</td><td>%d</td></tr>\r\n"), changeBoilerLevel);
    HtmlResponse.printf(F("<tr><td>Change at time</td><td>%s</td></tr>\r\n"), timeString);
    formatTime(timeString, sizeof(timeString), "%H:%M", currentTime);
    HtmlResponse.printf(F("<tr><td>currentBoilerLevelt time</td><td>%s</td></tr>\r\n"), timeString);
    HtmlResponse.println(F("</table>"));

    HtmlResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), eventLog.count());
    HtmlResponse.printf(F("<p class=\"log\"><a href=\"/log\">%d OpenTherm log entries.</a></p>\r\n"), openThermLog.count());

    writeHtmlFooter();

    WebServer.send(200, "text/html", HtmlResponse);
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer("handleHttpOpenThermLogRequest");

    writeHtmlHeader("OpenTherm log", false, true, true);
    
    HtmlResponse.println(F("<h2>Thermostat requests</h2>"));
    HtmlResponse.println(F("<table>"));
    for (int i = 0; i < 256; i++)
    {
        uint16_t thermostatRequest = thermostatRequests[i];
        if (thermostatRequest != DATA_VALUE_NONE)
            HtmlResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, thermostatRequest, getDecimal(thermostatRequest));
    }
    HtmlResponse.println(F("</table>"));

    HtmlResponse.println(F("<h2>Boiler responses</h2>"));
    HtmlResponse.println(F("<table>"));
    for (int i = 0; i < 256; i++)
    {
      uint16_t boilerResponse = boilerResponses[i];
      if (boilerResponse != DATA_VALUE_NONE)
          HtmlResponse.printf(F("<tr><td>%d</td><td>0x%04X</td><td>%0.2f</td></tr>\r\n"), i, boilerResponse, getDecimal(boilerResponse));
    }
    HtmlResponse.println(F("</table>"));

    HtmlResponse.println(F("<h2>OpenTherm log</h2>"));
    HtmlResponse.println(F("<table>"));
    HtmlResponse.println(F("<tr><td>Time</td><td>Thermostat TSet</td><td>Thermostat Max mod %</td><td>Boiler TSet</td><td>TBoiler</td></tr>"));
    char timeString[8];
    OpenThermLogEntry* otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getFirstEntry());
    while (otLogEntryPtr != NULL)
    {
        formatTime(timeString, sizeof(timeString), "%F %H:%M", otLogEntryPtr->time);
        HtmlResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td<td>%0.1f</td></tr>\r\n"), 
            timeString, 
            getDecimal(otLogEntryPtr->thermostatTSet), 
            getDecimal(otLogEntryPtr->thermostatMaxRelModulation), 
            getDecimal(otLogEntryPtr->boilerTSet), 
            getDecimal(otLogEntryPtr->boilerTWater)
            );

        otLogEntryPtr = static_cast<OpenThermLogEntry*>(openThermLog.getNextEntry());
    }
    HtmlResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HtmlResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");

    writeHtmlHeader("Event log", false, true, true);

    char* event = static_cast<char*>(eventLog.getFirstEntry());
    while (event != NULL)
    {
        HtmlResponse.printf(F("<div>%s</div>\r\n"), event);
        event = static_cast<char*>(eventLog.getNextEntry());
    }

    HtmlResponse.println(F("<div><a href=\"/events/clear\">Clear event log</a></div>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HtmlResponse);
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

    writeHtmlHeader("Configuration", false, true, true);

    HtmlResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HtmlResponse.println(F("<table>"));
    addTextBoxRow(HtmlResponse, "hostName", PersistentData.HostName, "Host name");
    addTextBoxRow(HtmlResponse, "tzOffset", tzOffsetString, "Timezone offset");
    HtmlResponse.println(F("</table>"));
    HtmlResponse.println(F("<input type=\"submit\">"));
    HtmlResponse.println(F("</form>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HtmlResponse);
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
