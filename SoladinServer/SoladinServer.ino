#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <Soladin.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include "PersistentData.h"
#include "WiFiCredentials.private.h"

 // Use same baud rate for debug output as ROM boot code
#define DEBUG_BAUDRATE 74880
// 36 seconds poll interval => 100 polls per hour
#define POLL_INTERVAL 36
#define MIN_NIGHT_DURATION (4 * 3600)
#define MAX_EVENT_LOG_SIZE 100
#define MAX_BAR_LENGTH 50
#define ICON "/apple-touch-icon.png"
#define NTP_SERVER "fritz.box"
#define FTP_SERVER "fritz.box"
#define FTP_RETRY_INTERVAL 3600

const float POLLS_PER_HOUR = 3600 / POLL_INTERVAL;

struct EnergyLogEntry
{
    time_t time;
    float energy = 0.0;
};


SoladinComm Soladin;
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, 24 * 3600); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
Log EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

Log EnergyPerHourLog(15);
Log EnergyPerDayLog(7);
Log EnergyPerWeekLog(10);
Log EnergyPerMonthLog(12);

EnergyLogEntry* energyPerHourLogEntryPtr = NULL;
EnergyLogEntry* energyPerDayLogEntryPtr = NULL;
EnergyLogEntry* energyPerWeekLogEntryPtr = NULL;
EnergyLogEntry* energyPerMonthLogEntryPtr = NULL;

bool soladinIsOn = false;
time_t initTime = 0;
time_t currentTime = 0;
time_t pollSoladinTime = 0;
time_t soladinLastOnTime = 0;
time_t maxPowerTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;
float lastGridEnergy = 0;
int maxPower = 0;

char timeString[32];


const char* formatTime(const char* format, time_t time)
{
    strftime(timeString, sizeof(timeString), format, gmtime(&time));
    return timeString;
}


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


void initializeHour()
{
    Tracer tracer(F("initializeHour"));
    
    EnergyLogEntry* energyPerHourLogEntryPtr = new EnergyLogEntry();
    energyPerHourLogEntryPtr->time = currentTime;

    EnergyPerHourLog.add(energyPerHourLogEntryPtr);
}


// Boot code
void setup() 
{
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    Serial.begin(DEBUG_BAUDRATE);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    
    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiState::Initialized, onWiFiInitialized);
    WiFiSM.begin(WIFI_SSID, WIFI_PASSWORD, PersistentData.hostName);

    Tracer::traceFreeHeap();
    
    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, 0);
        handleSerialRequest();
        digitalWrite(LED_BUILTIN, 1);
    }

    WiFiSM.run();

    delay(10);
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();
    initTime = currentTime;
    pollSoladinTime = currentTime;

    initializeDay();
    initializeWeek();
    initializeMonth();
}


void onWiFiInitialized()
{
    if (currentTime >= pollSoladinTime)
    {
        pollSoladinTime = currentTime + POLL_INTERVAL;
        digitalWrite(LED_BUILTIN, 0);
        pollSoladin();
        digitalWrite(LED_BUILTIN, 1);
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
    {
        if (trySyncFTP(NULL))
        {
            logEvent("FTP synced");
            syncFTPTime = 0;
        }
        else
        {
            logEvent("FTP sync failed");
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void initializeDay()
{
    Tracer tracer(F("initializeDay"));
    
    maxPower = 0;
    maxPowerTime = 0;

    EnergyPerHourLog.clear();
    initializeHour();

    EnergyLogEntry* energyPerDayLogEntryPtr = new EnergyLogEntry();
    energyPerDayLogEntryPtr->time = currentTime;

    EnergyPerDayLog.add(energyPerDayLogEntryPtr);
}


void initializeWeek()
{
    Tracer tracer(F("initializeWeek"));

    EnergyLogEntry* energyPerWeekLogEntryPtr = new EnergyLogEntry();
    energyPerWeekLogEntryPtr->time = currentTime;

    EnergyPerWeekLog.add(energyPerWeekLogEntryPtr);
}


void initializeMonth()
{
    Tracer tracer(F("initializeMonth"));

    EnergyLogEntry* energyPerMonthLogEntryPtr = new EnergyLogEntry();
    energyPerMonthLogEntryPtr->time = currentTime;

    EnergyPerMonthLog.add(energyPerMonthLogEntryPtr);
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

    if (!FTPClient.begin(FTP_SERVER, FTP_USERNAME, FTP_PASSWORD, FTP_DEFAULT_CONTROL_PORT, printTo))
    {
        FTPClient.end();
        return false;
    }

    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (EnergyPerHourLog.count() > 1)
        {
            EnergyLogEntry* energyLogEntryPtr = static_cast<EnergyLogEntry*>(EnergyPerDayLog.getEntryFromEnd(2));
            if (energyLogEntryPtr != NULL)
            {
                dataClient.printf(
                    "\"%s\",%0.2f\r\n",
                    formatTime("%F", energyLogEntryPtr->time),
                    energyPerDayLogEntryPtr->energy
                    );
            }
        }
        else
            TRACE("Nothing to sync.\n");
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            TRACE(F("FTP Append command failed: %s\n"), FTPClient.getLastResponse());
    }

    FTPClient.end();

    return success;
}


void pollSoladin()
{
    Tracer tracer(F("pollSoladin"));

    soladinIsOn = Soladin.getDeviceStats();
    if (!soladinIsOn)
    {
        TRACE(F("Soladin is off.\n"));
        return;
    }
    
    if (currentTime > (soladinLastOnTime + MIN_NIGHT_DURATION))
    {
        initializeDay();

        if (currentTime >= (energyPerWeekLogEntryPtr->time + 604800))
            initializeWeek();

        struct tm* now = gmtime(&currentTime);
        struct tm* monthLog = gmtime(&energyPerMonthLogEntryPtr->time);

        if (now->tm_mon != monthLog->tm_mon)
            initializeMonth();

        // Try to sync last day entry with FTP server
        syncFTPTime = currentTime;
    }
    else if (currentTime >= energyPerHourLogEntryPtr->time + 3600)
        initializeHour();

    soladinLastOnTime = currentTime;

    if (Soladin.gridPower > maxPower)
    {
        maxPower = Soladin.gridPower;
        maxPowerTime = currentTime;
    }

    float gridEnergyDelta = (lastGridEnergy == 0)  ? 0 : (Soladin.gridEnergy - lastGridEnergy); // kWh
    lastGridEnergy = Soladin.gridEnergy;
    TRACE(F("gridEnergyDelta = %f kWh"), gridEnergyDelta);
    
    energyPerHourLogEntryPtr->energy += float(Soladin.gridPower) / POLLS_PER_HOUR; // This has higher resolution than gridEnergyDelta
    energyPerDayLogEntryPtr->energy += gridEnergyDelta;
    energyPerWeekLogEntryPtr->energy += gridEnergyDelta; 
    energyPerMonthLogEntryPtr->energy += gridEnergyDelta; 

    TRACE(F("energyPerHour = %f\n"), energyPerHourLogEntryPtr->energy);

    if (Soladin.flags.length() > 0)
        logEvent(Soladin.flags);
}


void webTest()
{
    Tracer tracer(F("webTest"));

    // Simulate multiple incoming root requests
    for (int i = 0; i < 100; i++)
    {
        handleHttpRootRequest();
        yield();
    }
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");
    Serial.setTimeout(10);

    char cmd;
    if (!Serial.readBytes(&cmd, 1)) return;
    Serial.println(cmd);

    if (cmd == 't')
        for (int i = 0; i < 100; i++) 
        {
            Soladin.probe();
            yield();
        }
    else if (cmd == 'p')
        Soladin.probe();
    else if (cmd == 's')
        Soladin.getDeviceStats();
    else if (cmd == 'w')
        webTest();
    else if (cmd == 'f')
        trySyncFTP(NULL);
}


void writeHtmlHeader(String title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.hostName, title.c_str());
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), ICON, ICON);
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
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


void writeHtmlRow(String label, float value, String unitOfMeasure, const char* valueFormat = "%d")
{
    char valueString[16];
    snprintf(valueString, sizeof(valueString), valueFormat, value);

    HttpResponse.printf(
        F("<tr><td>%s</td><td>%s %s</td></tr>\r\n"),
        label.c_str(),
        valueString,
        unitOfMeasure.c_str()
        );
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    float pvPower = Soladin.pvVoltage * Soladin.pvCurrent;

    String status;
    if (soladinIsOn)
    {
        if (Soladin.flags.length() == 0)
            status = F("On");
        else
            status = Soladin.flags;
    }
    else
        status = F("Off");

    writeHtmlHeader(F("Home"), false, false);

    HttpResponse.println(F("<h1>Soladin device stats</h1>"));
    HttpResponse.println(F("<table class=\"devstats\">"));
    HttpResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), status.c_str());
    writeHtmlRow(F("PV Voltage"), Soladin.pvVoltage, "V", "%0.1f");
    writeHtmlRow(F("PV Current"), Soladin.pvCurrent, "A", "%0.2f");
    writeHtmlRow(F("PV Power"), pvPower, "W", "%0.1f");
    writeHtmlRow(F("Grid Voltage"), Soladin.gridVoltage, "V");
    writeHtmlRow(F("Grid Frequency"), Soladin.gridFrequency, "Hz", "%0.2f");
    writeHtmlRow(F("Grid Power"), Soladin.gridPower, "W");
    writeHtmlRow(F("Grid Energy"), Soladin.gridEnergy, "kWh", "%0.2f");
    writeHtmlRow(F("Temperature"), Soladin.temperature, "C");
    if (pvPower > 0)
        writeHtmlRow(F("Efficiency"), float(Soladin.gridPower) / pvPower * 100, "%", "%0.0f");
    writeHtmlRow(F("Max Grid Power"), maxPower, "W");
    if (maxPowerTime > 0)
        HttpResponse.printf(F("<tr><td>Max Power Time</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", maxPowerTime));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Soladin server status</h1>"));
    HttpResponse.println(F("<table class=\"devstats\">"));
    HttpResponse.printf(F("<tr><td>Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f days</td></tr>\r\n"), float(currentTime - initTime) / 86400);
    if (lastFTPSyncTime != 0)
        HttpResponse.printf(F("<tr><td>FTP Sync</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastFTPSyncTime));
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), EventLog.count());

    writeEnergyLogTable(F("Energy per hour"), EnergyPerHourLog, "%H:%M", 500, "Wh");
    writeEnergyLogTable(F("Energy per day"), EnergyPerDayLog, "%a", 5, "kWh");
    writeEnergyLogTable(F("Energy per week"), EnergyPerWeekLog, "%d %b", 35, "kWh");
    writeEnergyLogTable(F("Energy per month"), EnergyPerMonthLog, "%b", 150, "kWh");

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void writeGraphRow(String label, float value, float maxValue, String unitOfMeasure)
{
    int barLength = std::round((value / maxValue) * MAX_BAR_LENGTH);
    if (barLength > MAX_BAR_LENGTH)
        barLength = MAX_BAR_LENGTH;

    char bar[MAX_BAR_LENGTH + 1];
    memset(bar, 'o', barLength);
    bar[barLength] = 0;  

    HttpResponse.printf(
        F("<tr><td>%s</td><td>%0.2f %s</td><td><span class=\"bar\">%s</span></td></tr>\r\n"), 
        label.c_str(),
        value,
        unitOfMeasure.c_str(),
        bar
        );
}


void writeEnergyLogTable(String title, Log& energyLog, const char* labelFormat, float maxValue, String unitOfMeasure)
{
    HttpResponse.printf(F("<h1>%s</h1>"), title.c_str());
    HttpResponse.println(F("<table class=\"nrg\">"));

    EnergyLogEntry* energyLogEntryPtr = static_cast<EnergyLogEntry*>(energyLog.getFirstEntry());
    while (energyLogEntryPtr != NULL)
    {
        writeGraphRow(
            formatTime(labelFormat, energyLogEntryPtr->time),
            energyLogEntryPtr->energy,
            maxValue, unitOfMeasure
            );
        energyLogEntryPtr = static_cast<EnergyLogEntry*>(energyLog.getNextEntry());
    }

    HttpResponse.println(F("</table>"));
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


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    char tzOffsetString[4];
    sprintf(tzOffsetString, "%d", PersistentData.timeZoneOffset);

    writeHtmlHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("hostName"), PersistentData.hostName, F("Host name"));
    addTextBoxRow(HttpResponse, F("tzOffset"), tzOffsetString, F("Timezone offset"));
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    String tzOffsetString = WebServer.arg("tzOffset");

    strcpy(PersistentData.hostName, WebServer.arg("hostName").c_str()); 

    PersistentData.timeZoneOffset = static_cast<int8_t>(atoi(tzOffsetString.c_str()));

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
