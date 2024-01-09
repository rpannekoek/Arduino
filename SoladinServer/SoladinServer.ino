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
#include <Navigation.h>
#include "PersistentData.h"
#include "EnergyLogEntry.h"

constexpr int DEBUG_BAUDRATE = 74880; // Same as ROM boot output

constexpr int  POLL_INTERVAL = 36; // 100 polls per hour
constexpr float POLL_INTERVAL_HOURS = float(POLL_INTERVAL) / SECONDS_PER_HOUR;
constexpr int TODAY_LOG_INTERVAL = 30 * SECONDS_PER_MINUTE;
constexpr int MIN_NIGHT_DURATION = 4 * SECONDS_PER_HOUR;
constexpr int MAX_EVENT_LOG_SIZE = 50;
constexpr int MAX_BAR_LENGTH = 50;
constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int WIFI_TIMEOUT_MS = 2000;

#define SHOW_ENERGY "showEnergy"
#define TODAY F("today")
#define DAY F("day")
#define WEEK F("week")
#define MONTH F("month")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
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
    "Home.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

const char* Units[] PROGMEM = 
{
    "today",
    "day",
    "week",
    "month"
};

SoladinComm Soladin;
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
StringBuilder HttpResponse(16 * 1024); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], MAX_BAR_LENGTH);
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Navigation Nav;

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
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
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

    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

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
            WiFiSM.logEvent(F("FTP sync"));
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
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
    if (PersistentData.isFTPEnabled())
        syncFTPTime = currentTime;
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

    energyTodayLogEntryPtr->update(Soladin.gridPower, POLL_INTERVAL_HOURS, false);
    energyPerDayLogEntryPtr->update(Soladin.gridPower, POLL_INTERVAL_HOURS, true);
    energyPerWeekLogEntryPtr->update(Soladin.gridPower, POLL_INTERVAL_HOURS, true);
    energyPerMonthLogEntryPtr->update(Soladin.gridPower, POLL_INTERVAL_HOURS, true);

    if (Soladin.flags.length() > 0)
        WiFiSM.logEvent("Soladin: %s",Soladin.flags.c_str());
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
        TRACE(F("FTPClient.getLastError(): %s\n"), FTPClient.getLastError());
    }
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }
    
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

    Html.writeHeader(F("Home"), Nav, POLL_INTERVAL);

    const char* ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("FTP Sync"), ftpSync);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();
    Html.writeRow(F("Status"), status);
    Html.writeRow(F("U<sub>pv</sub>"), F("%0.1f V"), Soladin.pvVoltage);
    Html.writeRow(F("I<sub>pv</sub>"), F("%0.2f A"), Soladin.pvCurrent);
    Html.writeRow(F("P<sub>pv</sub>"), F("%0.1f W"), pvPower);
    Html.writeRow(F("U<sub>grid</sub>"), F("%d V"), Soladin.gridVoltage);
    Html.writeRow(F("F<sub>grid</sub>"), F("%0.2f Hz"), Soladin.gridFrequency);
    Html.writeRow(F("P<sub>grid</sub>"), F("%d W"), Soladin.gridPower);
    Html.writeRow(F("E<sub>grid</sub>"), F("%0.2f kWh"), Soladin.gridEnergy);
    Html.writeRow(F("Temperature"), F("%d °C"), Soladin.temperature);
    if (pvPower > 0)
        Html.writeRow(F("Efficiency"), F("%0.0f %%"), float(Soladin.gridPower) / pvPower * 100);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    String showEnergy = WebServer.hasArg(SHOW_ENERGY) ? WebServer.arg(SHOW_ENERGY) : TODAY;
    if (showEnergy == TODAY)
        writeEnergyLogTable(showEnergy, EnergyTodayLog, "%H:%M", "Wh");
    if (showEnergy == DAY)
        writeEnergyLogTable(showEnergy, EnergyPerDayLog, "%a", "kWh");
    if (showEnergy == WEEK)
        writeEnergyLogTable(showEnergy, EnergyPerWeekLog, "%d %b", "kWh");
    if (showEnergy == MONTH)
        writeEnergyLogTable(showEnergy, EnergyPerMonthLog, "%b", "kWh");

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeGraphRow(EnergyLogEntry* energyLogEntryPtr, const char* timeFormat, float maxValue)
{
    Html.writeRowStart();
    Html.writeCell(formatTime(timeFormat, energyLogEntryPtr->time));
    Html.writeCell(formatTimeSpan(static_cast<int>(energyLogEntryPtr->onDuration * 3600)));
    Html.writeCell(energyLogEntryPtr->maxPower);
    Html.writeCell(F("%0.2f"), energyLogEntryPtr->energy);
    Html.writeGraphCell(energyLogEntryPtr->energy / maxValue, F("energyBar"), false);
    Html.writeRowEnd();
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
    if (unit == TODAY)
        Html.writeHeaderCell(F("Time"));
    else
        Html.writeHeaderCell(unit);
    Html.writeHeaderCell(F("On time"));
    Html.writeHeaderCell(F("P<sub>max</sub> (W)"));
    HttpResponse.printf(F("<th>E (%s)</th>"), unitOfMeasure);
    Html.writeRowEnd();

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

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), Nav);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse); 
    Html.writePreEnd();

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
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
        Html.writeDiv(F("%s"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"), F("grid"));
    PersistentData.writeHtmlForm(Html);
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

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    PersistentData.parseHtmlFormData([](const String& id) -> const String& { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
