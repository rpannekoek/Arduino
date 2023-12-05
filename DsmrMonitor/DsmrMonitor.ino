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

#define REFRESH_INTERVAL 30
#define FTP_RETRY_INTERVAL (15 * 60)
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY (3600 * 24)
#define MAX_POWER_LOG_SIZE 250
#define POWER_LOG_PAGE_SIZE 50
#define POWER_LOG_INTERVAL 60

#define LED_ON 0
#define LED_OFF 1
#define P1_ENABLE 5

#define SHOW_ENERGY "showEnergy"
#define HOUR F("hour")
#define DAY F("day")
#define WEEK F("week")
#define MONTH F("month")

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_FTP_SYNC_ENTRIES F("FTPSyncEntries")
#define CFG_MAX_CURRENT F("MaxCurrent")
#define CFG_IS_3PHASE F("Is3Phase")
#define CFG_GAS_CALORIFIC F("GasCalorific")
#define CFG_POWER_LOG_DELTA F("PowerLogDelta")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ContentTypeJson = "application/json";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
    CalibrateIcon,
    GraphIcon,
    HomeIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    _LastFile
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Calibrate.svg",
    "Graph.svg",
    "Home.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

const char* Units[] PROGMEM = 
{
    "hour",
    "day",
    "week",
    "month"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], 45);
Log<const char> EventLog(50); // Max 50 log entries
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Navigation Nav;

P1Telegram LastP1Telegram;
StaticLog<PowerLogEntry> PowerLog(MAX_POWER_LOG_SIZE);
StaticLog<EnergyLogEntry> EnergyPerHourLog(24);
StaticLog<EnergyLogEntry> EnergyPerDayLog(7);
StaticLog<EnergyLogEntry> EnergyPerWeekLog(12);
StaticLog<EnergyLogEntry> EnergyPerMonthLog(12);

PowerLogEntry* powerLogEntryPtr = nullptr;
EnergyLogEntry* energyPerHourLogEntryPtr = nullptr;
EnergyLogEntry* energyPerDayLogEntryPtr = nullptr;
EnergyLogEntry* energyPerWeekLogEntryPtr = nullptr;
EnergyLogEntry* energyPerMonthLogEntryPtr = nullptr;

uint32_t lastTelegramReceivedMillis = 0;
time_t lastTelegramReceivedTime = 0;
time_t currentTime = 0;
time_t updatePowerLogTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

PhaseData phaseData[3];
PhaseData total;
GasData gasData;
int logEntriesToSync = 0;


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

    Nav.menuItems =
    {
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = PSTR("Home"),
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[LogFileIcon],
            .label = PSTR("Event log"),
            .urlPath =PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Power log"),
            .urlPath = PSTR("powerlog"),
            .handler = handleHttpPowerLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
        },
        MenuItem
        {
            .icon = Files[CalibrateIcon],
            .label = PSTR("P1 Telegram"),
            .urlPath = PSTR("telegram"),
            .handler= handleHttpViewTelegramRequest
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath =PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/json", handleHttpJsonRequest);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    memset(phaseData, 0, sizeof(phaseData));
    phaseData[0].label = F("L1");
    phaseData[1].label = F("L2");
    phaseData[2].label = F("L3");
    total.label = F("Total");

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();
}


void newEnergyPerHourLogEntry()
{
    Tracer tracer(F("newEnergyPerHourLogEntry"));
    
    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime - currentTime % SECONDS_PER_HOUR;

    energyPerHourLogEntryPtr = EnergyPerHourLog.add(&newEnergyLogEntry);
}


void newEnergyPerDayLogEntry()
{
    Tracer tracer(F("newEnergyPerDayLogEntry"));

    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime - currentTime % SECONDS_PER_DAY;

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

    total.voltage = (phaseData[0].voltage + phaseData[1].voltage + phaseData[2].voltage) / PersistentData.phaseCount;
    total.current = phaseData[0].current + phaseData[1].current + phaseData[2].current;
    total.powerDelivered = phaseData[0].powerDelivered + phaseData[1].powerDelivered + phaseData[2].powerDelivered;
    total.powerReturned = phaseData[0].powerReturned + phaseData[1].powerReturned + phaseData[2].powerReturned;

    String gasTimestamp;
    float gasEnergy = p1Telegram.getFloatValue(P1Telegram::PropertyId::Gas, &gasTimestamp) * PersistentData.gasCalorificValue;
    TRACE(F("Gas: %0.3f kWh @ %s.\n"), gasEnergy, gasTimestamp.c_str());
    if (gasTimestamp != gasData.timestamp)
        gasData.update(gasTimestamp, currentTime, gasEnergy);

    energyPerHourLogEntryPtr->update(
        total.powerDelivered,
        total.powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1 // Wh
        );  

    energyPerDayLogEntryPtr->update(
        total.powerDelivered,
        total.powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1000 // kWh
        );

    energyPerWeekLogEntryPtr->update(
        total.powerDelivered,
        total.powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1000 // kWh
        );

    energyPerMonthLogEntryPtr->update(
        total.powerDelivered,
        total.powerReturned,
        gasData.power,
        hoursSinceLastUpdate,
        1000 // kWh
        );
}


void updatePowerLog(time_t time)
{
    Tracer tracer(F("updatePowerLog"));

    if ((powerLogEntryPtr == nullptr)
        || (time >= powerLogEntryPtr->time + SECONDS_PER_HOUR)
        || isPowerDeltaThresholdExceeded())
    {
        PowerLogEntry newPowerLogEntry;
        newPowerLogEntry.time = time;
        for (int phase = 0; phase < 3; phase++)
        {
            newPowerLogEntry.powerDelivered[phase] = phaseData[phase].getAvgPowerDelivered();
            newPowerLogEntry.powerReturned[phase] = phaseData[phase].getAvgPowerReturned();
            phaseData[phase].reset();
        }
        newPowerLogEntry.powerGas = gasData.power;

        powerLogEntryPtr = PowerLog.add(&newPowerLogEntry);

        logEntriesToSync = std::min(logEntriesToSync + 1, MAX_POWER_LOG_SIZE);
        if (PersistentData.isFTPEnabled() && (logEntriesToSync == PersistentData.ftpSyncEntries))
            syncFTPTime = currentTime;
    }
    else
    {
        // Reset averages so it's a moving average over the last minute (POWER_LOG_INTERVAL)
        // Otherwise sudden changes get averaged out too much.
        for (int phase = 0; phase < 3; phase++)
        {
            phaseData[phase].reset();
        }
    }
}


bool isPowerDeltaThresholdExceeded()
{
    for (int phase = 0; phase < 3; phase++)
    {
        if (std::abs(phaseData[phase].getAvgPowerDelivered() - powerLogEntryPtr->powerDelivered[phase]) >= PersistentData.powerLogDelta)
            return true;
        if (std::abs(phaseData[phase].getAvgPowerReturned() - powerLogEntryPtr->powerReturned[phase]) >= PersistentData.powerLogDelta)
            return true;
    }

    return std::abs(gasData.power - powerLogEntryPtr->powerGas) >= PersistentData.powerLogDelta;
}


void testFillLogs()
{
    Tracer tracer(F("testFillLogs"));

    for (int hour = 0; hour <= 24; hour++)
    {
        newEnergyPerHourLogEntry();
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
        newEnergyPerDayLogEntry();
        energyPerDayLogEntryPtr->time += day * SECONDS_PER_DAY;
        energyPerDayLogEntryPtr->energyDelivered = day;
        energyPerDayLogEntryPtr->energyReturned = 7 - day;
        energyPerDayLogEntryPtr->energyGas = day;
    }

    time_t time = currentTime;
    for (int i = 0; i < MAX_POWER_LOG_SIZE; i++)
    {
        float f = float(i) / 1000;
        phaseData[0].update(230, 10, f * 10, f * 10 + 1);
        phaseData[1].update(231, 5, f * 5, f * 5 + 1);
        phaseData[2].update(232, 1, f, f + 1);
        gasData.power = i * 2;
        updatePowerLog(time);
        time += POWER_LOG_INTERVAL;
    }
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();

    updatePowerLogTime = currentTime + POWER_LOG_INTERVAL;

    newEnergyPerHourLogEntry();
    newEnergyPerDayLogEntry();
    newEnergyPerWeekLogEntry();
    newEnergyPerMonthLogEntry();

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
    if (currentTime >= energyPerHourLogEntryPtr->time + SECONDS_PER_HOUR)
    {
        newEnergyPerHourLogEntry();

        if (currentTime >= energyPerDayLogEntryPtr->time + SECONDS_PER_DAY)
        {
            newEnergyPerDayLogEntry();

            if (currentTime >= energyPerWeekLogEntryPtr->time + (SECONDS_PER_DAY * 7))
                newEnergyPerWeekLogEntry();

            int currentMonth = gmtime(&currentTime)->tm_mon;
            int lastLogMonth = gmtime(&energyPerMonthLogEntryPtr->time)->tm_mon;
            if (currentMonth != lastLogMonth)
                newEnergyPerMonthLogEntry();
        }
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
            WiFiSM.logEvent(message);

        if (message.startsWith(F("/testFill")))
            testFillLogs();
        else if (!message.startsWith(F("ERROR")))
        {
            float hoursSinceLastUpdate = float(millisSinceLastTelegram) / 3600000;
            updateStatistics(LastP1Telegram, hoursSinceLastUpdate);
        }
    }

    if (currentTime >= updatePowerLogTime)
    {
        updatePowerLogTime += POWER_LOG_INTERVAL;
        updatePowerLog(currentTime);
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent(F("FTP sync"));
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
            syncFTPTime = currentTime + FTP_RETRY_INTERVAL;
        }
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

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
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (logEntriesToSync > 0)
        {
            PowerLogEntry* firstLogEntryPtr = PowerLog.getEntryFromEnd(logEntriesToSync);
            writeCsvPowerLogEntries(firstLogEntryPtr, dataClient);
            logEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println(F("Nothing to sync."));
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            FTPClient.setUnexpectedResponse();
    }

    FTPClient.end();

    return success;
}


void writeCsvPowerLogHeader(Print& destination)
{
    destination.print(F("Time;"));
    for (int phase = 1; phase <= PersistentData.phaseCount; phase++)
        destination.printf("Pd%d (W);Pr%d (W);", phase, phase);
    destination.println("Pgas (W)");
}


void writeCsvPowerLogEntries(PowerLogEntry* logEntryPtr, Print& destination)
{
    while (logEntryPtr != nullptr)
    {
        destination.print(formatTime("%F %H:%M", logEntryPtr->time));
        for (int phase = 0; phase < PersistentData.phaseCount; phase++)
            destination.printf(";%d;%d", logEntryPtr->powerDelivered[phase], logEntryPtr->powerReturned[phase]);
        destination.printf(";%d\r\n", logEntryPtr->powerGas);
        
        logEntryPtr = PowerLog.getNextEntry();
    }
}


void writeHtmlPhaseData(PhaseData& phaseData, float maxPower)
{
    Html.writeRowStart();
    Html.writeHeaderCell(phaseData.label);
    Html.writeCell(phaseData.voltage, F("%0.1f V"));
    Html.writeCell(phaseData.current, F("%0.0f A"));
    HttpResponse.printf(
        F("<td><div>+%0.0f W</div><div>-%0.0f W</div></td>"),
        phaseData.powerDelivered,
        phaseData.powerReturned
        );
    Html.writeCellStart(F("graph"));
    Html.writeBar(phaseData.powerDelivered / maxPower, F("deliveredBar"), true);
    Html.writeBar(phaseData.powerReturned / maxPower, F("returnedBar"), true);
    Html.writeCellEnd();
    Html.writeRowEnd();
}


void writeHtmlGasData(float maxPower)
{
    Html.writeRowStart();
    Html.writeHeaderCell(F("Gas"));
    HttpResponse.printf(F("<td colspan=\"2\">%0.1f kWh</td>"), gasData.energy);
    Html.writeCell(gasData.power, F("%0.0f W"));
    Html.writeCellStart(F("graph"));
    Html.writeBar(gasData.power / maxPower, F("gasBar"), true);
    Html.writeCellEnd();
    Html.writeRowEnd();
}


void writeHtmlEnergyRow(
    EnergyLogEntry* energyLogEntryPtr,
    const char* timeFormat,
    float maxValue)
{
    Html.writeRowStart();
    Html.writeCell(formatTime(timeFormat, energyLogEntryPtr->time));
    HttpResponse.printf(
        F("<td><div>+%d</div><div>-%d</div><div>%d</div></td>"),
        energyLogEntryPtr->maxPowerDelivered,
        energyLogEntryPtr->maxPowerReturned,
        energyLogEntryPtr->maxPowerGas);
    HttpResponse.printf(
        F("<td><div>+%0.1f</div><div>-%0.1f</div><div>%0.1f</div></td>"),
        energyLogEntryPtr->energyDelivered,
        energyLogEntryPtr->energyReturned,
        energyLogEntryPtr->energyGas);
    Html.writeCellStart(F("graph"));
    Html.writeBar(energyLogEntryPtr->energyDelivered / maxValue, F("deliveredBar"), false);
    Html.writeBar(energyLogEntryPtr->energyReturned / maxValue, F("returnedBar"), false);
    Html.writeBar(energyLogEntryPtr->energyGas / maxValue, F("gasBar"), false);
    Html.writeCellEnd();
    Html.writeRowEnd();
}


void writeHtmlEnergyLogTable(
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
        maxValue = std::max(maxValue, energyLogEntryPtr->energyDelivered);
        maxValue = std::max(maxValue, energyLogEntryPtr->energyReturned);
        maxValue = std::max(maxValue, energyLogEntryPtr->energyGas);
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    HttpResponse.println(F("<section>"));
    HttpResponse.printf(
        F("<h1>Energy per <span class=\"dropdown\"><span>%s</span>"),
        unit.c_str());
    Html.writeDivStart(F("dropdown-list"));
    for (int i = 0; i < 4; i++)
    {
        String u = FPSTR(Units[i]);
        if (u == unit) continue;
        HttpResponse.printf(F("<a href=\"?showEnergy=%s\">%s</a>\r\n"), u.c_str(), u.c_str());
    }
    Html.writeDivEnd();
    HttpResponse.println(F("</span></h1>"));

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(unit);
    Html.writeHeaderCell(F("P<sub>max</sub> (W)"));
    HttpResponse.printf(F("<th>E (%s)</th>"), unitOfMeasure);
    Html.writeRowEnd();

    energyLogEntryPtr = energyLog.getFirstEntry();
    while (energyLogEntryPtr != nullptr)
    {
        writeHtmlEnergyRow(energyLogEntryPtr, timeFormat, maxValue);
        energyLogEntryPtr = energyLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), Nav, REFRESH_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("Last Telegram"), formatTime("%H:%M:%S", lastTelegramReceivedTime));
    Html.writeRow(F("Gas update"), formatTime("%H:%M:%S", gasData.time));
    Html.writeRow(F("FTP Sync"), ftpSync);
    if (PersistentData.isFTPEnabled())
        Html.writeRow(F("Sync entries"), F("%d / %d"), logEntriesToSync, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    int maxPhasePower = 230 * PersistentData.maxPhaseCurrent; 
    int maxTotalPower = maxPhasePower * PersistentData.phaseCount;

    Html.writeSectionStart(F("Current power"));
    Html.writeTableStart();
    if (PersistentData.phaseCount == 3)
    {
        for (int i = 0; i < 3; i++)
            writeHtmlPhaseData(phaseData[i], maxPhasePower);
    }
    writeHtmlPhaseData(total, maxTotalPower);
    writeHtmlGasData(maxTotalPower);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    String showEnergy = WebServer.hasArg(SHOW_ENERGY) ? WebServer.arg(SHOW_ENERGY) : DAY;
    if (showEnergy == HOUR)
        writeHtmlEnergyLogTable(showEnergy, EnergyPerHourLog, "%H:%M", "Wh");
    if (showEnergy == DAY)
        writeHtmlEnergyLogTable(showEnergy, EnergyPerDayLog, "%a", "kWh");
    if (showEnergy == WEEK)
        writeHtmlEnergyLogTable(showEnergy, EnergyPerWeekLog, "%d %b", "kWh");
    if (showEnergy == MONTH)
        writeHtmlEnergyLogTable(showEnergy, EnergyPerMonthLog, "%b", "kWh");

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpJsonRequest()
{
    Tracer tracer(F("handleHttpJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ \"Electricity\": [ "));

    if (PersistentData.phaseCount == 3)
    {
        for (int i = 0; i < 3; i++)
        {
            if (i > 0) HttpResponse.print(F(", "));
            writeJsonPhaseData(phaseData[i]);
        }
        HttpResponse.print(F(", "));
    }

    writeJsonPhaseData(total);

    HttpResponse.printf(
        F(" ], \"Egas\": %0.1f, \"Pgas\": %0.0f }"),
        gasData.energy,
        gasData.power);

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void writeJsonPhaseData(PhaseData& phaseData)
{
    HttpResponse.printf(
        F("{ \"Phase\": \"%s\", \"U\": %0.1f, \"I\": %0.0f, \"Pdelivered\": %0.0f, \"Preturned\": %0.0f }"),
        phaseData.label.c_str(),
        phaseData.voltage,
        phaseData.current,
        phaseData.powerDelivered,
        phaseData.powerReturned);
}


void handleHttpViewTelegramRequest()
{
    Tracer tracer(F("handleHttpViewTelegramRequest"));

    Html.writeHeader(F("P1 Telegram"), Nav, REFRESH_INTERVAL);
    
    HttpResponse.printf(
        F("<p>Received %d data lines at %s:</p>"),
        LastP1Telegram._numDataLines,
        formatTime("%H:%M:%S", lastTelegramReceivedTime)
        );

    HttpResponse.println(F("<pre>"));

    for (int i = 0; i < LastP1Telegram._numDataLines; i++)
    {
        HttpResponse.print(LastP1Telegram._dataLines[i]);
    }

    HttpResponse.println(F("</pre>"));
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpPowerLogRequest()
{
    Tracer tracer(F("handleHttpPowerLogRequest"));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((PowerLog.count() - 1) / POWER_LOG_PAGE_SIZE) + 1;

    Html.writeHeader(F("Power log"), Nav);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    for (int i = 1; i <= PersistentData.phaseCount; i++)
        HttpResponse.printf(F("<th>Pd%d (W)</th><th>Pr%d (W)</th>"), i, i);
    Html.writeHeaderCell(F("Pgas (W)"));
    Html.writeRowEnd();

    PowerLogEntry* logEntryPtr = PowerLog.getFirstEntry();
    for (int i = 0; i < (currentPage * POWER_LOG_PAGE_SIZE) && logEntryPtr != nullptr; i++)
    {
        logEntryPtr = PowerLog.getNextEntry();
    }
    for (int i = 0; i < POWER_LOG_PAGE_SIZE && logEntryPtr != nullptr; i++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        for (int phase = 0; phase < PersistentData.phaseCount; phase++)
        {
            Html.writeCell(logEntryPtr->powerDelivered[phase]);
            Html.writeCell(logEntryPtr->powerReturned[phase]);
        }
        Html.writeCell(logEntryPtr->powerGas);
        Html.writeRowEnd();

        logEntryPtr = PowerLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), Nav);

    HttpResponse.println("<pre>");
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println("</pre>");

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());

    Html.writeHeading(F("CSV header"), 2);
    HttpResponse.print(F("<pre>"));
    writeCsvPowerLogHeader(HttpResponse);
    HttpResponse.println(F("</pre>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    Html.writeHeader(F("Event log"), Nav);

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), "Clear event log", currentTime, ButtonClass);
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"), F("grid"));
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1, F("password"));
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1, F("password"));
    Html.writeNumberBox(CFG_FTP_SYNC_ENTRIES, F("FTP sync entries"), PersistentData.ftpSyncEntries, 0, MAX_POWER_LOG_SIZE);
    Html.writeNumberBox(CFG_MAX_CURRENT, F("Max current"), PersistentData.maxPhaseCurrent, 0, 50);
    Html.writeCheckbox(CFG_IS_3PHASE, F("Three phases"), (PersistentData.phaseCount == 3));
    Html.writeNumberBox(CFG_GAS_CALORIFIC, F("Gas kWh per m3"), PersistentData.gasCalorificValue, 8, 12, 3);
    Html.writeNumberBox(CFG_POWER_LOG_DELTA, F("Power log delta"), PersistentData.powerLogDelta, 0, 100);
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

    String maxCurrent = WebServer.arg(CFG_MAX_CURRENT);
    String isThreePhase = WebServer.arg(CFG_IS_3PHASE);
    String gasCalorific = WebServer.arg(CFG_GAS_CALORIFIC);
    String powerLogDelta = WebServer.arg(CFG_POWER_LOG_DELTA);

    PersistentData.maxPhaseCurrent = maxCurrent.toInt();
    PersistentData.phaseCount = (isThreePhase == "true") ? 3 : 1;
    PersistentData.gasCalorificValue = gasCalorific.toFloat();
    PersistentData.powerLogDelta = powerLogDelta.toInt();
    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_SYNC_ENTRIES).toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
