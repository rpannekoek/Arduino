#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include "PersistentData.h"
#include "PhaseData.h"
#include "GasData.h"
#include "EnergyLogEntry.h"
#include "PowerLogEntry.h"
#include "P1Telegram.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define REFRESH_INTERVAL 30
#define FTP_RETRY_INTERVAL 3600
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY (3600 * 24)
#define MAX_POWER_LOG_SIZE 250

#define LED_ON 0
#define LED_OFF 1
#define P1_ENABLE 5

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_MAX_CURRENT F("MaxCurrent")
#define CFG_IS_3PHASE F("Is3Phase")
#define CFG_GAS_CALORIFIC F("GasCalorific")
#define CFG_POWER_LOG_DELTA F("PowerLogDelta")

const char* ContentTypeHtml = "text/html;charset=UTF-8";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 60); // Max bar length: 60
Log<const char> EventLog(50); // Max 50 log entries
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

P1Telegram LastP1Telegram;
PowerLogEntry PowerLog[MAX_POWER_LOG_SIZE];
Log<EnergyLogEntry> EnergyPerHourLog(25); // 24 + 1 so we can FTP sync daily
Log<EnergyLogEntry> EnergyPerDayLog(7);
EnergyLogEntry* energyPerHourLogEntryPtr = nullptr;
EnergyLogEntry* energyPerDayLogEntryPtr = nullptr;

uint32_t lastTelegramReceivedMillis = 0;
time_t lastTelegramReceivedTime = 0;
time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

PhaseData phaseData[3];
GasData gasData;
int logEntriesToSync = 0;
bool isFTPEnabled = false;
int powerLogIndex = 0;


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


// Boot code
void setup() 
{
    // Turn built-in LED on during boot
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    // Disable P1 interface
    digitalWrite(P1_ENABLE, 0);
    pinMode(P1_ENABLE, OUTPUT);

    // ESMR 5.0: 115200 8N1, telegram each second (approx. 900 bytes)
    Serial.setTimeout(1500); // Timeout allows for skipping one telegram
    Serial.setRxBufferSize(1024); // Ensure RX buffer fits full telegram
    Serial.begin(115200);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
    Html.setTitlePrefix(PersistentData.hostName);
    isFTPEnabled = PersistentData.ftpServer[0] != 0;

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/telegram", handleHttpViewTelegramRequest);
    WebServer.on("/powerlog", handleHttpPowerLogRequest);
    WebServer.on("/powerlog/clear", handleHttpPowerLogClearRequest);
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
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    memset(phaseData, 0, sizeof(phaseData));

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();
    WiFiSM.run();
    delay(10);
}


void initializeDay()
{
    Tracer tracer(F("initializeDay"));

    powerLogIndex = -1;

    energyPerDayLogEntryPtr = new EnergyLogEntry();
    // Set entry time to start of day (00:00)
    energyPerDayLogEntryPtr->time = currentTime - currentTime % SECONDS_PER_DAY;

    EnergyPerDayLog.add(energyPerDayLogEntryPtr);
}


void initializeHour()
{
    Tracer tracer(F("initializeHour"));
    
    energyPerHourLogEntryPtr = new EnergyLogEntry();
    // Set entry time to start of hour (xy:00)
    energyPerHourLogEntryPtr->time = currentTime - currentTime % SECONDS_PER_HOUR;

    EnergyPerHourLog.add(energyPerHourLogEntryPtr);
}


void updateStatistics(P1Telegram& p1Telegram, float hoursSinceLastUpdate)
{
    Tracer tracer(F("updateStatistics"));

    phaseData[0].update(
        p1Telegram.getFloatValue(P1Telegram::PropertyId::VoltageL1),
        p1Telegram.getFloatValue(P1Telegram::PropertyId::CurrentL1),
        p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerDeliveredL1),
        p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerReturnedL1)
        );

    if (PersistentData.phaseCount == 3)
    {
        phaseData[1].update(
            p1Telegram.getFloatValue(P1Telegram::PropertyId::VoltageL2),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::CurrentL2),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerDeliveredL2),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerReturnedL2)
            );

        phaseData[2].update(
            p1Telegram.getFloatValue(P1Telegram::PropertyId::VoltageL3),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::CurrentL3),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerDeliveredL3),
            p1Telegram.getFloatValue(P1Telegram::PropertyId::PowerReturnedL3)
            );
    }

    updatePowerLog();

    String gasTimestamp;
    float gasEnergy = p1Telegram.getFloatValue(P1Telegram::PropertyId::Gas, &gasTimestamp) * PersistentData.gasCalorificValue;
    TRACE(F("Gas: %0.3f kWh @ %s.\n"), gasEnergy, gasTimestamp.c_str());
    if (gasTimestamp != gasData.timestamp)
        gasData.update(gasTimestamp, currentTime, gasEnergy);

    float powerDelivered = phaseData[0].powerDelivered + phaseData[1].powerDelivered + phaseData[2].powerDelivered;
    float powerReturned = phaseData[0].powerReturned + phaseData[1].powerReturned + phaseData[2].powerReturned;    

    energyPerHourLogEntryPtr->update(
        powerDelivered,
        powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1 // Wh
        );  

    energyPerDayLogEntryPtr->update(
        powerDelivered,
        powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1000 // kWh
        );
}


void updatePowerLog()
{
    if (powerLogIndex == MAX_POWER_LOG_SIZE - 1)
        return; // Log is full

    bool needNewLogEntry = powerLogIndex < 0;
    if (!needNewLogEntry) 
        needNewLogEntry =  exceedsDeltaThreshold(0) || exceedsDeltaThreshold(1) || exceedsDeltaThreshold(2); 
    if (needNewLogEntry)
    {
        PowerLogEntry& powerLogEntry = PowerLog[++powerLogIndex];
        powerLogEntry.time = currentTime;
        for (int phase = 0; phase < 3; phase++)
            powerLogEntry.power[phase] = phaseData[phase].powerDelivered;
    }
}


bool exceedsDeltaThreshold(int phase)
{
    return std::abs(phaseData[phase].powerDelivered - PowerLog[powerLogIndex].power[phase]) >= PersistentData.powerLogDelta;
}


void testFillLogs()
{
    Tracer tracer(F("testFillLogs"));

    for (int hour = 0; hour <= 24; hour++)
    {
        initializeHour();
        energyPerHourLogEntryPtr->time += hour * SECONDS_PER_HOUR;
        energyPerHourLogEntryPtr->maxPowerDelivered = hour * 10;
        energyPerHourLogEntryPtr->maxPowerReturned = 240 - hour * 10;
        energyPerHourLogEntryPtr->maxPowerGas = 2400 / (hour + 1);
        energyPerHourLogEntryPtr->energyDelivered = hour;
        energyPerHourLogEntryPtr->energyReturned = 24 - hour;
        energyPerHourLogEntryPtr->energyGas = hour;
    }

    for (int day = 0; day <= 7; day++)
    {
        initializeDay();
        energyPerDayLogEntryPtr->time += day * SECONDS_PER_DAY;
        energyPerDayLogEntryPtr->energyDelivered = day;
        energyPerDayLogEntryPtr->energyReturned = 7 - day;
        energyPerDayLogEntryPtr->energyGas = day;
    }

    logEntriesToSync = 24;
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();

    initializeDay();
    initializeHour();

    // Flush any garbage from Serial input
    while (Serial.available())
    {
        Serial.read();
    }

    // Enable P1 interface
    digitalWrite(P1_ENABLE, 1);
}


void onWiFiInitialized()
{
    if (currentTime >= energyPerDayLogEntryPtr->time + SECONDS_PER_DAY)
        initializeDay();
    if (currentTime >= energyPerHourLogEntryPtr->time + SECONDS_PER_HOUR)
    {
        initializeHour();
        if ((++logEntriesToSync == 24) && isFTPEnabled)
        {
            syncFTPTime = currentTime;
        }
        if (logEntriesToSync > 24) logEntriesToSync = 24;
    }

    if (Serial.available())
    {
        uint32_t currentMillis = millis();
        int32_t millisSinceLastTelegram = (lastTelegramReceivedMillis == 0) ? 0 :  currentMillis - lastTelegramReceivedMillis; 
        if (millisSinceLastTelegram < 0)
        {
            // millis() rollover
            millisSinceLastTelegram = 0;
        }
        lastTelegramReceivedMillis = currentMillis;
        lastTelegramReceivedTime = currentTime;

        // Turn on built-in LED while receiving P1 telegram
        digitalWrite(LED_BUILTIN, LED_ON);
        String message = LastP1Telegram.readFrom(Serial); 
        digitalWrite(LED_BUILTIN, LED_OFF);

        if (message.length() > 0)
            logEvent(message);

        if (message.startsWith(F("/testFill")))
            testFillLogs();
        else if (!message.startsWith(F("ERROR")))
        {
            float hoursSinceLastUpdate = float(millisSinceLastTelegram) / 3600000;
            updateStatistics(LastP1Telegram, hoursSinceLastUpdate);
        }
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
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


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    if (!isFTPEnabled)
    {
        logEvent(F("No FTP server configured.\n"));
        return false;
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

    if (!FTPClient.begin(PersistentData.ftpServer, PersistentData.ftpUser, PersistentData.ftpPassword, FTP_DEFAULT_CONTROL_PORT, printTo))
    {
        FTPClient.end();
        return false;
    }

    bool success = false;
    if (logEntriesToSync == 0)
        success = true;
    else
    {
        WiFiClient& dataClient = FTPClient.append(filename);
        if (dataClient.connected())
        {
            writeCsvDataLines(EnergyPerHourLog, dataClient);
            dataClient.stop();

            if (FTPClient.readServerResponse() == 226)
            {
                TRACE(F("Successfully appended log entries.\n"));
                logEntriesToSync = 0;
                lastFTPSyncTime = currentTime;
                success = true;
            }
            else
                TRACE(F("FTP Append command failed: %s\n"), FTPClient.getLastResponse());
        }
    }

    FTPClient.end();

    return success;
}


void writeCsvDataLines(Log<EnergyLogEntry>& energyLog, Print& destination)
{
    // Do not include the last entry, which is still in progress.
    EnergyLogEntry* energyLogEntryPtr = energyLog.getEntryFromEnd(logEntriesToSync + 1);
    int i = logEntriesToSync;
    while ((energyLogEntryPtr != nullptr) && (i-- > 0))
    {
        destination.printf(
            "\"%s\";%d;%d;%d;%0.0f;%0.0f;%0.0f\r\n", 
            formatTime("%F %H:%M", energyLogEntryPtr->time),
            energyLogEntryPtr->maxPowerDelivered,
            energyLogEntryPtr->maxPowerReturned,
            energyLogEntryPtr->maxPowerGas,
            energyLogEntryPtr->energyDelivered,
            energyLogEntryPtr->energyReturned,
            energyLogEntryPtr->energyGas
            );
        
        energyLogEntryPtr = energyLog.getNextEntry();
    }
}


void writeCsvPowerLog(Print& destination)
{
    for (int i = 0; i <= powerLogIndex; i++)
    {
        PowerLogEntry& powerLogEntry = PowerLog[i];
        destination.printf(
            "%s;%d;%d;%d\r\n", 
            formatTime("%H:%M:%S", powerLogEntry.time),
            powerLogEntry.power[0],
            powerLogEntry.power[1],
            powerLogEntry.power[2]
            );
    }
}


void writeHtmlPhaseData(String label,PhaseData& phaseData, float maxPower)
{
    HttpResponse.printf(F("<tr><th>%s</th>"), label.c_str());
    HttpResponse.printf(F("<td>%0.1f V</td>"), phaseData.voltage);
    HttpResponse.printf(F("<td>%0.0f A</td>"), phaseData.current);
    HttpResponse.printf(
        F("<td><div>+%0.0f W</div><div>-%0.0f W</div></td><td class=\"graph\">"),
        phaseData.powerDelivered,
        phaseData.powerReturned
        );

    Html.writeBar(phaseData.powerDelivered / maxPower, F("deliveredBar"), true);
    Html.writeBar(phaseData.powerReturned / maxPower, F("returnedBar"), true);

    HttpResponse.println(F("</td></tr>"));
}


void writeHtmlGasData(float maxPower)
{
    HttpResponse.printf(
        F("<tr><th>Gas</th><td></td><td></td><td>%0.0f W</td><td class=\"graph\">"),
        gasData.power
        );

    Html.writeBar(gasData.power / maxPower, F("gasBar"), true);

    HttpResponse.println(F("</td></tr>"));
}


void writeHtmlEnergyRow(
    EnergyLogEntry* energyLogEntryPtr,
    const char* labelFormat,
    float maxValue,
    float hours
    )
{
    const char* unitOfMeasure = (hours == 1) ?  "Wh" : "kWh";    

    HttpResponse.printf(F("<tr><td>%s</td>"), formatTime(labelFormat, energyLogEntryPtr->time));

    HttpResponse.printf(
        F("<td><div>+%d W max</div><div>-%d W max</div><div>%d W max</div></td>"),
        energyLogEntryPtr->maxPowerDelivered,
        energyLogEntryPtr->maxPowerReturned,
        energyLogEntryPtr->maxPowerGas
        );

    HttpResponse.printf(
        F("<td><div>+%0.1f %s</div><div>-%0.1f %s</div><div>%0.1f %s</div></td><td class=\"graph\">"),
        energyLogEntryPtr->energyDelivered,
        unitOfMeasure,
        energyLogEntryPtr->energyReturned,
        unitOfMeasure,
        energyLogEntryPtr->energyGas,
        unitOfMeasure
        );

    Html.writeBar(energyLogEntryPtr->energyDelivered / maxValue, F("deliveredBar"), false);
    Html.writeBar(energyLogEntryPtr->energyReturned / maxValue, F("returnedBar"), false);
    Html.writeBar(energyLogEntryPtr->energyGas / maxValue, F("gasBar"), false);

    HttpResponse.println(F("</td></tr>")); 
}


void writeHtmlEnergyLogTable(String title, Log<EnergyLogEntry>& energyLog, const char* labelFormat, int hours)
{
    // Auto-ranging: determine max value from the log entries
    float maxValue = 0;
    EnergyLogEntry* energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        if (energyLogEntryPtr->energyDelivered > maxValue) maxValue  = energyLogEntryPtr->energyDelivered;
        if (energyLogEntryPtr->energyReturned > maxValue) maxValue  = energyLogEntryPtr->energyReturned;
        if (energyLogEntryPtr->energyGas > maxValue) maxValue  = energyLogEntryPtr->energyGas;
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    HttpResponse.printf(F("<h1>%s</h1>"), title.c_str());
    HttpResponse.println(F("<table class=\"nrg\">"));

    energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        writeHtmlEnergyRow(
            energyLogEntryPtr,
            labelFormat,
            maxValue,
            hours
            );
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    HttpResponse.println(F("</table>"));
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    PhaseData total;
    total.voltage = (phaseData[0].voltage + phaseData[1].voltage + phaseData[2].voltage) / PersistentData.phaseCount;
    total.current = phaseData[0].current + phaseData[1].current + phaseData[2].current;
    total.powerDelivered = phaseData[0].powerDelivered + phaseData[1].powerDelivered + phaseData[2].powerDelivered;
    total.powerReturned = phaseData[0].powerReturned + phaseData[1].powerReturned + phaseData[2].powerReturned;
    
    int maxPhasePower = 230 * PersistentData.maxPhaseCurrent; 
    int maxTotalPower = maxPhasePower * PersistentData.phaseCount;

    Html.writeHeader(F("Home"), false, false, REFRESH_INTERVAL);

    HttpResponse.println(F("<h1>Current power</h1>"));
    HttpResponse.println(F("<p><table class=\"power\">"));
    if (PersistentData.phaseCount == 3)
    {
        writeHtmlPhaseData(F("L1"), phaseData[0], maxPhasePower);
        writeHtmlPhaseData(F("L2"), phaseData[1], maxPhasePower);
        writeHtmlPhaseData(F("L3"), phaseData[2], maxPhasePower);
    }
    writeHtmlPhaseData(F("Total"), total, maxTotalPower);
    writeHtmlGasData(maxTotalPower);
    HttpResponse.println(F("</table></p>"));

    HttpResponse.println(F("<h1>Monitor status</h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><td><a href=\"/telegram\">Last Telegram</a></td><td>%s</td></tr>\r\n"), formatTime("%H:%M:%S", lastTelegramReceivedTime));
    HttpResponse.printf(F("<tr><td>Last Gas Time</td><td>%s</td></tr>\r\n"), formatTime("%H:%M:%S", gasData.time));
    if (isFTPEnabled)
    {
        HttpResponse.printf(F("<tr><td>FTP Sync Time</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastFTPSyncTime));
        HttpResponse.printf(F("<tr><td><a href=\"/sync\">FTP Sync entries</a></td><td>%d</td></tr>\r\n"), logEntriesToSync);
    }
    else
    {
        HttpResponse.println(F("<tr><td>FTP Sync</td><td>Disabled</td></tr>"));
    }
    
    HttpResponse.printf(F("<tr><td><a href=\"/powerlog\">Power entries</a></td><td>%d</td></tr>\r\n"), (powerLogIndex + 1));
    HttpResponse.printf(F("<tr><td><a href=\"/events\">Events logged</a></td><td>%d</td></tr>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    writeHtmlEnergyLogTable(F("Energy per hour"), EnergyPerHourLog, "%H:%M", 1);
    writeHtmlEnergyLogTable(F("Energy per day"), EnergyPerDayLog, "%a", 24);

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpViewTelegramRequest()
{
    Tracer tracer(F("handleHttpViewTelegramRequest"));

    Html.writeHeader(F("P1 Telegram"), true, true, REFRESH_INTERVAL);
    
    HttpResponse.printf(
        F("<p>Received %d data lines at %s:</p>"),
        LastP1Telegram._numDataLines,
        formatTime("%H:%M:%S", lastTelegramReceivedTime)
        );
    HttpResponse.println(F("<pre class=\"telegram\">"));

    for (int i = 0; i < LastP1Telegram._numDataLines; i++)
    {
        HttpResponse.print(LastP1Telegram._dataLines[i]);
    }

    HttpResponse.println(F("</pre>"));
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpPowerLogRequest()
{
    Tracer tracer(F("handleHttpPowerLogRequest"));

    Html.writeHeader(F("Power Log"), true, true, REFRESH_INTERVAL);

    HttpResponse.println(F("<p><a href=\"/powerlog/clear\">Clear log</a></p>"));
    HttpResponse.printf(F("<p>%d log entries (delta: %d W):</p>\r\n"), (powerLogIndex + 1), PersistentData.powerLogDelta);

    HttpResponse.println(F("<pre class=\"powerlog\">"));
    HttpResponse.println(F("Time;P1 (W);P2 (W);P3 (W)"));

    writeCsvPowerLog(HttpResponse);

    HttpResponse.println(F("</pre>"));
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpPowerLogClearRequest()
{
    Tracer tracer(F("handleHttpPowerLogClearRequest"));
    powerLogIndex = -1;
    handleHttpPowerLogRequest();
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), true, true);

    HttpResponse.println("<div><pre class=\"ftplog\">");
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

    Html.writeHeader(F("Event log"), true, true, REFRESH_INTERVAL);

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
    Html.writeTextBox(CFG_MAX_CURRENT, F("Max current"), String(PersistentData.maxPhaseCurrent), 2);
    Html.writeCheckbox(CFG_IS_3PHASE, F("Three phases"), (PersistentData.phaseCount == 3));
    Html.writeTextBox(CFG_GAS_CALORIFIC, F("Gas kWh per m3"), String(PersistentData.gasCalorificValue, 3), 6);
    Html.writeTextBox(CFG_POWER_LOG_DELTA, F("Power log delta"), String(PersistentData.powerLogDelta), 4);
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

    String maxCurrent = WebServer.arg(CFG_MAX_CURRENT);
    String isThreePhase = WebServer.arg(CFG_IS_3PHASE);
    String gasCalorific = WebServer.arg(CFG_GAS_CALORIFIC);
    String powerLogDelta = WebServer.arg(CFG_POWER_LOG_DELTA);

    PersistentData.maxPhaseCurrent = maxCurrent.toInt();
    PersistentData.phaseCount = (isThreePhase == "true") ? 3 : 1;
    PersistentData.gasCalorificValue = gasCalorific.toFloat();
    PersistentData.powerLogDelta = powerLogDelta.toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
