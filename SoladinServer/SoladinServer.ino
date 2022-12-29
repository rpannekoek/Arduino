#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include "Soladin.h"
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include <HtmlWriter.h>
#include "PersistentData.h"
#include "EnergyLogEntry.h"
#include "WiFiCredentials.private.h"

 // Use same baud rate for debug output as ROM boot code
#define DEBUG_BAUDRATE 74880
// 36 seconds poll interval => 100 polls per hour
#define POLL_INTERVAL 36
#define TODAY_LOG_INTERVAL (30 * 60)
#define MIN_NIGHT_DURATION (4 * 3600)
#define MAX_EVENT_LOG_SIZE 50
#define MAX_BAR_LENGTH 50
#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"
#define NTP_SERVER "fritz.box"
#define FTP_SERVER "fritz.box"
#define FTP_RETRY_INTERVAL 3600
#define WIFI_TIMEOUT_MS 2000

#define SHOW_ENERGY "showEnergy"
#define TODAY F("today")
#define DAY F("day")
#define WEEK F("week")
#define MONTH F("month")

const float pollIntervalHours = float(POLL_INTERVAL) / 3600;
const char* ContentTypeHtml = "text/html;charset=UTF-8";

SoladinComm Soladin;
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, 24 * 3600); // Synchronize daily
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, MAX_BAR_LENGTH);
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

StaticLog<EnergyLogEntry> EnergyTodayLog(16 * 2); // 16 hours ontime max
StaticLog<EnergyLogEntry> EnergyPerDayLog(7);
StaticLog<EnergyLogEntry> EnergyPerWeekLog(12);
StaticLog<EnergyLogEntry> EnergyPerMonthLog(12);

EnergyLogEntry* energyTodayLogEntryPtr = nullptr;
EnergyLogEntry* energyPerDayLogEntryPtr = nullptr;
EnergyLogEntry* energyPerWeekLogEntryPtr = nullptr;
EnergyLogEntry* energyPerMonthLogEntryPtr = nullptr;

bool soladinIsOn = false;
time_t currentTime = 0;
time_t pollSoladinTime = 0;
time_t soladinLastOnTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;
float lastGridEnergy = 0;


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

    Serial.begin(DEBUG_BAUDRATE);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    Html.setTitlePrefix(PersistentData.hostName);
    
    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/sync", handleHttpSyncFTPRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
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

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();
}


void onTimeServerSynced()
{
    currentTime = TimeServer.getCurrentTime();
    pollSoladinTime = currentTime;
    soladinLastOnTime = currentTime; // Prevent immediate new day

    newEnergyTodayLogEntry();
    newEnergyPerDayLogEntry();
    newEnergyPerWeekLogEntry();
    newEnergyPerMonthLogEntry();
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

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            logEvent(F("FTP synchronized"));
            syncFTPTime = 0;
        }
        else
        {
            String event = F("FTP sync failed: ");
            event += FTPClient.getLastResponse();
            logEvent(event);
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void newEnergyTodayLogEntry()
{
    Tracer tracer(F("newEnergyTodayLogEntry"));
    
    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = (currentTime - (currentTime % TODAY_LOG_INTERVAL));
 
    energyTodayLogEntryPtr = EnergyTodayLog.add(&newEnergyLogEntry);
}


void newEnergyPerDayLogEntry()
{
    Tracer tracer(F("newEnergyPerDayLogEntry"));
    
    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime;
 
    energyPerDayLogEntryPtr = EnergyPerDayLog.add(&newEnergyLogEntry);
}


void newEnergyPerWeekLogEntry()
{
    Tracer tracer(F("newEnergyPerWeekLogEntry"));

    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime;
 
    energyPerWeekLogEntryPtr = EnergyPerWeekLog.add(&newEnergyLogEntry);
}


void newEnergyPerMonthLogEntry()
{
    Tracer tracer(F("newEnergyPerMonthLogEntry"));

    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime;
 
    energyPerMonthLogEntryPtr = EnergyPerMonthLog.add(&newEnergyLogEntry);
}


void startNewDay()
{
    Tracer tracer(F("startNewDay"));

    EnergyTodayLog.clear();
    newEnergyTodayLogEntry();

    newEnergyPerDayLogEntry();

    if (currentTime >= (energyPerWeekLogEntryPtr->time + 561600)) // use 6.5 days to deal with sunrise earlier than last week
        newEnergyPerWeekLogEntry();

    int currentMonth = gmtime(&currentTime)->tm_mon;
    int lastLogMonth = gmtime(&energyPerMonthLogEntryPtr->time)->tm_mon;
    if (currentMonth != lastLogMonth)
        newEnergyPerMonthLogEntry();

    // Try to sync last day entry with FTP server
    syncFTPTime = currentTime;
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
        if (EnergyPerDayLog.count() > 1)
        {
            EnergyLogEntry* energyLogEntryPtr = EnergyPerDayLog.getEntryFromEnd(2);
            if (energyLogEntryPtr != nullptr)
            {
                dataClient.printf(
                    "\"%s\",%0.1f,%u,%0.2f\r\n",
                    formatTime("%F", energyLogEntryPtr->time),
                    energyLogEntryPtr->onDuration,
                    energyLogEntryPtr->maxPower,
                    energyLogEntryPtr->energy
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
        startNewDay();
    else if (currentTime >= energyTodayLogEntryPtr->time + TODAY_LOG_INTERVAL)
        newEnergyTodayLogEntry();

    soladinLastOnTime = currentTime;

    energyTodayLogEntryPtr->update(Soladin.gridPower, pollIntervalHours, false);
    energyPerDayLogEntryPtr->update(Soladin.gridPower, pollIntervalHours, true);
    energyPerWeekLogEntryPtr->update(Soladin.gridPower, pollIntervalHours, true);
    energyPerMonthLogEntryPtr->update(Soladin.gridPower, pollIntervalHours, true);

    if (Soladin.flags.length() > 0)
        logEvent(Soladin.flags);
}


void webTest()
{
    Tracer tracer(F("webTest"));

    // Create some test data
    for (int i = 0; i < 7; i++)
    {
        newEnergyPerDayLogEntry();
        energyPerDayLogEntryPtr->energy = i;
        energyPerDayLogEntryPtr->onDuration = i;
        energyPerDayLogEntryPtr->maxPower = i;
    }

    for (int i = 0; i < 16; i++)
    {
        newEnergyTodayLogEntry();
        energyTodayLogEntryPtr->energy = i;
        energyTodayLogEntryPtr->onDuration = i;
        energyTodayLogEntryPtr->maxPower = i;
    }

    for (int i = 0; i < 12; i++)
    {
        newEnergyPerWeekLogEntry();
        energyPerWeekLogEntryPtr->energy = i;
        energyPerWeekLogEntryPtr->onDuration = i;
        energyPerWeekLogEntryPtr->maxPower = i;
    }

    for (int i = 0; i < 12; i++)
    {
        newEnergyPerMonthLogEntry();
        energyPerMonthLogEntryPtr->energy = i;
        energyPerMonthLogEntryPtr->onDuration = i;
        energyPerMonthLogEntryPtr->maxPower = i;
    }

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
    {
        currentTime += 32 * 24 * 3600;
        startNewDay();
    }
    else if (cmd == 'p')
        Soladin.probe();
    else if (cmd == 's')
        Soladin.getDeviceStats();
    else if (cmd == 'w')
        webTest();
    else if (cmd == 'f')
    {
        trySyncFTP(nullptr);
        TRACE(F("FTPClient.getLastResponse(): %s\n"), FTPClient.getLastResponse());
    }
}


void writeHtmlRow(String label, float value, String unitOfMeasure, const char* valueFormat = "%0.0f")
{
    char valueString[16];
    snprintf(valueString, sizeof(valueString), valueFormat, value);

    HttpResponse.printf(
        F("<tr><th>%s</th><td>%s %s</td></tr>\r\n"),
        label.c_str(),
        valueString,
        unitOfMeasure.c_str()
        );
}


void writeEnergyLink(String unit)
{
    HttpResponse.printf(F(" <a href=\"?%s=%s\">%s</a>"), SHOW_ENERGY, unit.c_str(), unit.c_str());
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

    Html.writeHeader(F("Home"), false, false, POLL_INTERVAL);

    const char* ftpSync;
    if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    HttpResponse.println(F("<h1>Soladin server status</h1>"));
    HttpResponse.println(F("<table class=\"devstats\">"));
    HttpResponse.printf(F("<tr><th>RSSI</th><td>%d</td></tr>\r\n"), static_cast<int>(WiFi.RSSI()));
    HttpResponse.printf(F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th>FTP Sync</th><td>%s</td></tr>\r\n"), ftpSync);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged.</a></th><td>%d</td>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Soladin device stats</h1>"));
    HttpResponse.println(F("<table class=\"devstats\">"));
    HttpResponse.printf(F("<tr><th>Status</th><td>%s</td></tr>\r\n"), status.c_str());
    writeHtmlRow(F("PV Voltage"), Soladin.pvVoltage, "V", "%0.1f");
    writeHtmlRow(F("PV Current"), Soladin.pvCurrent, "A", "%0.2f");
    writeHtmlRow(F("PV Power"), pvPower, "W", "%0.1f");
    writeHtmlRow(F("Grid Voltage"), Soladin.gridVoltage, "V");
    writeHtmlRow(F("Grid Frequency"), Soladin.gridFrequency, "Hz", "%0.2f");
    writeHtmlRow(F("Grid Power"), Soladin.gridPower, "W");
    writeHtmlRow(F("Grid Energy"), Soladin.gridEnergy, "kWh", "%0.2f");
    writeHtmlRow(F("Temperature"), Soladin.temperature, "°C");
    if (pvPower > 0)
        writeHtmlRow(F("Efficiency"), float(Soladin.gridPower) / pvPower * 100, "%", "%0.0f");
    HttpResponse.println(F("</table>"));

    String showEnergy = WebServer.hasArg(SHOW_ENERGY) ? WebServer.arg(SHOW_ENERGY) : TODAY;
    HttpResponse.print(F("<p>Show energy per"));
    if (showEnergy != TODAY) writeEnergyLink(TODAY);
    if (showEnergy != DAY) writeEnergyLink(DAY);
    if (showEnergy != WEEK) writeEnergyLink(WEEK);
    if (showEnergy != MONTH) writeEnergyLink(MONTH);
    HttpResponse.println(F("</p>"));

    if (showEnergy == TODAY)
        writeEnergyLogTable(TODAY, EnergyTodayLog, "%H:%M", "Wh");
    if (showEnergy == DAY)
        writeEnergyLogTable(showEnergy, EnergyPerDayLog, "%a", "kWh");
    if (showEnergy == WEEK)
        writeEnergyLogTable(showEnergy, EnergyPerWeekLog, "%d %b", "kWh");
    if (showEnergy == MONTH)
        writeEnergyLogTable(showEnergy, EnergyPerMonthLog, "%b", "kWh");

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeGraphRow(EnergyLogEntry* energyLogEntryPtr, const char* timeFormat, float maxValue)
{
    HttpResponse.printf(
        F("<tr><td>%s</td><td>%s</td><td>%d</td><td>%0.2f</td><td class=\"graph\">"), 
        formatTime(timeFormat, energyLogEntryPtr->time),
        formatTimeSpan(static_cast<int>(energyLogEntryPtr->onDuration * 3600)),
        energyLogEntryPtr->maxPower,
        energyLogEntryPtr->energy
        );

    Html.writeBar(energyLogEntryPtr->energy / maxValue, F("energyBar"), false, false);

    HttpResponse.println(F("</td></tr>"));
}


void writeEnergyLogTable(
    String unit,
    StaticLog<EnergyLogEntry>& energyLog,
    const char* timeFormat,
    const char* unitOfMeasure)
{
    // Auto-ranging: determine max value from the log entries
    float maxValue = 1; // Prevent division by zero
    EnergyLogEntry* energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        maxValue = std::max(maxValue, energyLogEntryPtr->energy);
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    HttpResponse.printf(F("<h1>Energy per %s</h1>\r\n"), unit.c_str());
    HttpResponse.println(F("<table class=\"nrg\">"));
    HttpResponse.printf(
        F("<tr><th>%s</th><th>On time</th><th>P<sub>max</sub> (W)</th><th>E (%s)</th></tr>\r\n"),
        (unit == TODAY) ? "Time" : unit.c_str(),
        unitOfMeasure);

    energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        writeGraphRow(
            energyLogEntryPtr,
            timeFormat,
            maxValue
            );
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    HttpResponse.println(F("</table>"));
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), true, true);

    HttpResponse.println("<div><pre>");
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println("</pre></div>");

    if (success)
    {
        HttpResponse.println("<p>Success!</p>");
    }
    else
        HttpResponse.println("<p>Failed!</p>");
 
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    Html.writeHeader(F("Event log"), true, true);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
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

    Html.writeHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("hostName"), PersistentData.hostName, F("Host name"));
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

    copyString(WebServer.arg("hostName"), PersistentData.hostName, sizeof(PersistentData.hostName));

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
