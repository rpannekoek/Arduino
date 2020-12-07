#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <Soladin.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include <math.h>
#include "PersistentData.h"
#include "P1Telegram.h"
#include "WiFiCredentials.private.h"

#define REFRESH_INTERVAL 30
#define MAX_EVENT_LOG_SIZE 50
#define MAX_BAR_LENGTH 60
#define ICON "/apple-touch-icon.png"
#define NTP_SERVER "fritz.box"
#define FTP_SERVER "fritz.box"
#define FTP_RETRY_INTERVAL 3600

#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY (3600 * 24)

#define LED_ON 0
#define LED_OFF 1

struct PhaseData
{
    float voltage;
    float current;
    float powerDelivered;
    float powerReturned;

    void update(float newVoltage, float newCurrent, float newPowerDelivered, float newPowerReturned)
    {
        voltage = newVoltage;
        current = newCurrent;
        powerDelivered = newPowerDelivered * 1000; // Watts
        powerReturned = newPowerReturned * 1000; // Watts
    }
};

struct GasData
{
    String timestamp;
    time_t time = 0;
    float energy = 0; // kWh
    float power = 0;

    void update(String& newTimestamp, time_t newTime, float newEnergy)
    {
        timestamp = newTimestamp;
        if (time > 0)
        {
            float deltaEnergy = (newEnergy - energy) * 1000; // Wh
            float deltaTime = float(newTime - time) / SECONDS_PER_HOUR; // hours
            power = deltaEnergy / deltaTime;
            TRACE(F("Delta energy: %0.0f Wh in %f h. Power: %0.0f W\n"), deltaEnergy, deltaTime, power);
        }
        time = newTime;
        energy = newEnergy;
    }
};

struct EnergyLogEntry
{
    time_t time;
    uint16_t maxPowerDelivered = 0; // Watts
    uint16_t maxPowerReturned = 0; // Watts
    uint16_t maxPowerGas = 0; // Watts
    float energyDelivered = 0.0; // Wh or kWh
    float energyReturned = 0.0; // Wh or kWh
    float energyGas = 0.0; // Wh or kWh

    void update(
        float powerDelivered,
        float powerReturned,
        float powerGas,
        float hoursSinceLastUpdate,
        float scale
        )
    {
        TRACE(F("EnergyLogEntry::update(%0.0f, %0.0f, %0.0f, %f, %0.0f)\n"), 
            powerDelivered, powerReturned, powerGas, hoursSinceLastUpdate, scale);

        energyDelivered += powerDelivered * hoursSinceLastUpdate / scale;
        energyReturned += powerReturned * hoursSinceLastUpdate / scale;
        energyGas += powerGas * hoursSinceLastUpdate / scale;

        if (powerDelivered > maxPowerDelivered)
            maxPowerDelivered = powerDelivered;

        if (powerReturned > maxPowerReturned)
            maxPowerReturned = powerReturned;

        if (powerGas > maxPowerGas)
            maxPowerGas = powerGas;
    }
};

WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, SECONDS_PER_DAY); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

P1Telegram LastP1Telegram;
Log<EnergyLogEntry> EnergyPerHourLog(25); // 24 + 1 so we can FTP sync daily
Log<EnergyLogEntry> EnergyPerDayLog(7);

unsigned long lastTelegramReceivedMillis = 0;
time_t lastTelegramReceivedTime = 0;
time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

struct PhaseData phaseData[3];
struct GasData gasData;
EnergyLogEntry* energyPerHourLogEntryPtr = nullptr;
EnergyLogEntry* energyPerDayLogEntryPtr = nullptr;
int logEntriesToSync = 0;


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
    // Turn built-in LED on during boot
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    // ESMR 5.0: 115200 8N1
    Serial.setTimeout(1000);
    Serial.begin(115200);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    // TODO: Get TZ offset from P1 telegram

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/telegram", handleHttpViewTelegramRequest);
    WebServer.on("/sync", handleHttpSyncFTPRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiState::Initialized, onWiFiInitialized);
    WiFiSM.begin(WIFI_SSID, WIFI_PASSWORD, PersistentData.hostName);

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
    currentTime = TimeServer.getCurrentTime();

    initializeDay();
    initializeHour();
}


void onWiFiInitialized()
{
    if (currentTime >= energyPerDayLogEntryPtr->time + SECONDS_PER_DAY)
        initializeDay();
    if (currentTime >= energyPerHourLogEntryPtr->time + SECONDS_PER_HOUR)
    {
        initializeHour();
        if (++logEntriesToSync == 24)
        {
            syncFTPTime = currentTime;
        }
        if (logEntriesToSync > 24) logEntriesToSync = 24;
    }

    if (Serial.available())
    {
        unsigned long currentMillis = millis();
        long millisSinceLastTelegram = (lastTelegramReceivedMillis == 0) ? 0 :  currentMillis - lastTelegramReceivedMillis; 
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

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

    if (!FTPClient.begin(FTP_SERVER, FTP_USERNAME, FTP_PASSWORD, FTP_DEFAULT_CONTROL_PORT, printTo))
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


void writeHtmlHeader(String title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.hostName, title.c_str());
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), ICON, ICON);
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , REFRESH_INTERVAL);
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


void writeHtmlBar(float value, String cssClass, bool fill)
{
    int barLength = round(value * MAX_BAR_LENGTH);
    barLength = min(barLength, MAX_BAR_LENGTH);

    char bar[MAX_BAR_LENGTH + 1];
    memset(bar, 'o', barLength);
    bar[barLength] = 0;

    HttpResponse.printf(F("<div><span class=\"%s\">%s</span>"), cssClass.c_str(), bar);

    if (fill)
    {
        memset(bar, 'o', MAX_BAR_LENGTH - barLength);
        bar[MAX_BAR_LENGTH - barLength] = 0;

        HttpResponse.printf(F("<span class=\"barFill\">%s</span>"), bar);
    }
    else if (barLength == 0)
    {
        // Ensure that an empty bar has the same height
        HttpResponse.print(F("<span class=\"emptyBar\">o</span>"));
    }
    

    HttpResponse.print("</div>");
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

    writeHtmlBar(phaseData.powerDelivered / maxPower, F("deliveredBar"), true);
    writeHtmlBar(phaseData.powerReturned / maxPower, F("returnedBar"), true);

    HttpResponse.println(F("</td></tr>"));
}


void writeHtmlGasData(float maxPower)
{
    HttpResponse.printf(
        F("<tr><th>Gas</th><td></td><td></td><td>%0.0f W</td><td class=\"graph\">"),
        gasData.power
        );

    writeHtmlBar(gasData.power / maxPower, F("gasBar"), true);

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
        F("<td><div>+%0.2f %s</div><div>-%0.2f %s</div><div>%0.2f %s</div></td><td class=\"graph\">"),
        energyLogEntryPtr->energyDelivered,
        unitOfMeasure,
        energyLogEntryPtr->energyReturned,
        unitOfMeasure,
        energyLogEntryPtr->energyGas,
        unitOfMeasure
        );

    writeHtmlBar(energyLogEntryPtr->energyDelivered / maxValue, F("deliveredBar"), false);
    writeHtmlBar(energyLogEntryPtr->energyReturned / maxValue, F("returnedBar"), false);
    writeHtmlBar(energyLogEntryPtr->energyGas / maxValue, F("gasBar"), false);

    HttpResponse.println(F("</td></tr>")); 
}


void writeHtmlEnergyLogTable(String title, Log<EnergyLogEntry>& energyLog, const char* labelFormat, int hours)
{
    // Auto-ranging: determine max value from the log entries
    float maxValue = 0;
    EnergyLogEntry* energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        if (energyLogEntryPtr->energyDelivered > maxValue)
            maxValue  = energyLogEntryPtr->energyDelivered;
        if (energyLogEntryPtr->energyReturned > maxValue)
            maxValue  = energyLogEntryPtr->energyReturned;
        if (energyLogEntryPtr->energyGas > maxValue)
            maxValue  = energyLogEntryPtr->energyGas;
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
    
    PhaseData total;
    total.voltage = (phaseData[0].voltage + phaseData[1].voltage + phaseData[2].voltage) / PersistentData.phaseCount;
    total.current = phaseData[0].current + phaseData[1].current + phaseData[2].current;
    total.powerDelivered = phaseData[0].powerDelivered + phaseData[1].powerDelivered + phaseData[2].powerDelivered;
    total.powerReturned = phaseData[0].powerReturned + phaseData[1].powerReturned + phaseData[2].powerReturned;
    
    int maxPhasePower = 230 * PersistentData.maxPhaseCurrent; 
    int maxTotalPower = maxPhasePower * PersistentData.phaseCount;

    writeHtmlHeader(F("Home"), false, false);

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
    HttpResponse.printf(F("<tr><td>FTP Sync Time</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastFTPSyncTime));
    HttpResponse.printf(F("<tr><td><a href=\"/sync\">FTP Sync Entries</a></td><td>%d</td></tr>\r\n"), logEntriesToSync);
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p><a href=\"/events\">%d events logged.</a></p>\r\n"), EventLog.count());

    writeHtmlEnergyLogTable(F("Energy per hour"), EnergyPerHourLog, "%H:%M", 1);
    writeHtmlEnergyLogTable(F("Energy per day"), EnergyPerDayLog, "%a", 24);

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpViewTelegramRequest()
{
    Tracer tracer(F("handleHttpViewTelegramRequest"));

    writeHtmlHeader("P1 Telegram", true, true);
    
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
    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    writeHtmlHeader("FTP Sync", true, true);

    HttpResponse.println("<div><pre>");
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println("</pre></div>");

    if (success)
    {
        HttpResponse.println("<p>Success!</p>");
    }
    else
        HttpResponse.println("<p>Failed!</p>");
 
    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    writeHtmlHeader(F("Event log"), true, true);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
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


void addCheckboxRow(StringBuilder& output, String name, bool value, String label)
{
    const char* checked = value ? "checked" : "";

    output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"checkbox\" name=\"%s\" value=\"true\" %s></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        checked
        );
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    char tzOffsetString[4];
    char maxCurrentString[4];
    char gasCalorificString[8];
    snprintf(tzOffsetString, sizeof(tzOffsetString), "%d", PersistentData.timeZoneOffset);
    snprintf(maxCurrentString, sizeof(maxCurrentString), "%d", PersistentData.maxPhaseCurrent);
    snprintf(gasCalorificString, sizeof(gasCalorificString), "%0.3f", PersistentData.gasCalorificValue);

    writeHtmlHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("hostName"), PersistentData.hostName, F("Host name"));
    addTextBoxRow(HttpResponse, F("tzOffset"), tzOffsetString, F("Timezone offset"));
    addTextBoxRow(HttpResponse, F("maxCurrent"), maxCurrentString, F("Max current"));
    addCheckboxRow(HttpResponse, F("isThreePhase"), (PersistentData.phaseCount == 3), F("Three phases"));
    addTextBoxRow(HttpResponse, F("gasCalorific"), gasCalorificString, F("Gas kWh per m3"));
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
    String maxCurrentString = WebServer.arg("maxCurrent");
    String isThreePhaseString = WebServer.arg("isThreePhase");
    String gasCalorificString = WebServer.arg("gasCalorific");

    strcpy(PersistentData.hostName, WebServer.arg("hostName").c_str()); 

    PersistentData.timeZoneOffset = tzOffsetString.toInt();
    PersistentData.maxPhaseCurrent = maxCurrentString.toInt();
    PersistentData.phaseCount = (isThreePhaseString == "true") ? 3 : 1;
    PersistentData.gasCalorificValue = gasCalorificString.toFloat();

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
