#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <AsyncHTTPRequest_Generic.h>
#include "PersistentData.h"
#include "Aquarea.h"
#include "MonitoredTopics.h"
#include "DayStatsEntry.h"
#include "OTGWClient.h"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define TOPIC_LOG_SIZE 200
#define TOPIC_LOG_PAGE_SIZE 50
#define DEFAULT_BAR_LENGTH 60
#define WIFI_TIMEOUT_MS 2000
#define FTP_RETRY_INTERVAL (15 * 60)
#define QUERY_AQUAREA_INTERVAL 6
#define AGGREGATION_INTERVAL 60
#define ANTI_FREEZE_DELTA_T 5
#define HP_ON_THRESHOLD 10
#define PUMP_FLOW_THRESHOLD 7

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_FTP_SYNC_ENTRIES F("FTPSyncEntries")
#define CFG_ANTI_FREEZE_TEMP F("AntiFreezeTemp")
#define CFG_LOG_PACKET_ERRORS F("LogPacketErrors")
#define CFG_ZONE1_OFFSET F("Zone1Offset")
#define CFG_OTGW_HOST F("OTGW")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ContentTypeJson = "application/json";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
    BinaryIcon,
    GraphIcon,
    HomeIcon,
    ListIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    _LastFile
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Binary.svg",
    "Graph.svg",
    "Home.svg",
    "List.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
OTGWClient OTGW;
StringBuilder HttpResponse(4 * 1024); // 4 kB HTTP response buffer (we're using chunked responses)
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], DEFAULT_BAR_LENGTH);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TopicLogEntry> TopicLog(TOPIC_LOG_SIZE);
StaticLog<DayStatsEntry> DayStats(7);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Aquarea HeatPump;
Navigation Nav;

TopicLogEntry newTopicLogEntry;
int topicLogAggregations = 0;

TopicLogEntry* lastTopicLogEntryPtr = nullptr;
DayStatsEntry* lastDayStatsEntryPtr = nullptr;

uint16_t ftpSyncEntries = 0;
uint16_t heatPumpOnCount = 0;
bool isDefrosting = false;
bool antiFreezeActivated = false;
bool testAntiFreeze = false;
bool testDefrost = false;
bool pumpPrime = false;
int otgwAttempt = 0;

time_t currentTime = 0;
time_t queryAquareaTime = 0;
time_t lastPacketReceivedTime = 0;
time_t lastPacketErrorTime = 0;
time_t topicLogAggregationTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;


// Boot code
void setup() 
{
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    Serial.begin(74880); // Use same baudrate as bootloader (will be switched by HeatPump.begin)
    Serial.println("Boot"); // Flush any garbage caused by ESP boot output.

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
            .label = PSTR("Aquarea log"),
            .urlPath = PSTR("topiclog"),
            .handler = handleHttpTopicLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpFtpSyncRequest
        },
        MenuItem
        {
            .icon = Files[ListIcon],
            .label = PSTR("Topics"),
            .urlPath = PSTR("topic"),
            .handler= handleHttpTopicsRequest
        },
        MenuItem
        {
            .icon = Files[BinaryIcon],
            .label = PSTR("Hex dump"),
            .urlPath = PSTR("hexdump"),
            .handler= handleHttpHexDumpRequest
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

    WebServer.on("/test", handleHttpTestRequest);
    WebServer.on("/json", handleHttpAquaMonJsonRequest);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.registerStaticFiles(Files, _LastFile);    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    HeatPump.setZone1Offset(PersistentData.zone1Offset);
    HeatPump.begin();

    if (PersistentData.otgwHost[0] != 0)
    {
        OTGW.begin(PersistentData.otgwHost);
    }

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


void newDayStatsEntry()
{
    time_t startOfDay = currentTime - (currentTime % SECONDS_PER_DAY);
    DayStatsEntry newDayStatsEntry;
    newDayStatsEntry.startTime = startOfDay;
    newDayStatsEntry.stopTime = startOfDay;
    lastDayStatsEntryPtr = DayStats.add(&newDayStatsEntry);
}


void onTimeServerSynced()
{
    queryAquareaTime = currentTime;
    topicLogAggregationTime = currentTime + AGGREGATION_INTERVAL;

    newTopicLogEntry.time = currentTime;
    newTopicLogEntry.reset();

    newDayStatsEntry();
}


void onWiFiInitialized()
{
    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, LED_ON);
        if (HeatPump.readPacket())
            handleNewAquareaData();
        else
        {
            lastPacketErrorTime = currentTime;
            if (PersistentData.logPacketErrors)
                WiFiSM.logEvent(HeatPump.getLastError());
        }
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if (currentTime >= queryAquareaTime)
    {
        queryAquareaTime += QUERY_AQUAREA_INTERVAL;
        if (!HeatPump.sendQuery())
            WiFiSM.logEvent(F("Failed sending Aquarea query"));
    }

    if (!WiFiSM.isConnected())
        return;

    if (OTGW.isInitialized && OTGW.isRequestPending())
    {
        int otgwResult = OTGW.requestData();
        if (otgwResult == HTTP_OK)
            WiFiSM.logEvent(F("OTGW: '%s'"), OTGW.boilerLevel);
        else if (otgwResult != HTTP_REQUEST_PENDING)
        {
            WiFiSM.logEvent(F("OTGW: %s (#%d)"), OTGW.getLastError().c_str(), ++otgwAttempt);
            if (otgwAttempt < 3)
                OTGW.retry();
        }
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
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


void handleNewAquareaData()
{
    Tracer tracer(F("handleNewAquareaData"));

    uint32_t secondsSinceLastUpdate = (lastPacketReceivedTime == 0) ? 0 : (currentTime - lastPacketReceivedTime);
    lastPacketReceivedTime = currentTime;

    float inletTemp = HeatPump.getTopic(TopicId::Main_Inlet_Temp).getValue().toFloat();
    float outletTemp = HeatPump.getTopic(TopicId::Main_Outlet_Temp).getValue().toFloat();
    float compPower = HeatPump.getTopic(TopicId::Compressor_Power).getValue().toFloat();
    float heatPower = HeatPump.getTopic(TopicId::Heat_Power).getValue().toFloat();
    int defrostingState = HeatPump.getTopic(TopicId::Defrosting_State).getValue().toInt();
    float pumpFlow = HeatPump.getTopic(TopicId::Pump_Flow).getValue().toFloat();

    if (testDefrost) defrostingState = 1;

    bool pumpIsOn = pumpFlow >= PUMP_FLOW_THRESHOLD;
    if (!pumpIsOn)
        heatPumpOnCount = 0;
    else if (compPower > 0)
        heatPumpOnCount++;

    if (OTGW.isInitialized && WiFiSM.isConnected())
        otgwPumpControl(pumpIsOn, defrostingState);
    antiFreezeControl(inletTemp, outletTemp, compPower);
    updateDayStats(secondsSinceLastUpdate, compPower, heatPower, defrostingState);
    updateTopicLog();

    isDefrosting = defrostingState != 0;
}


bool otgwSetPump(bool on, const String& reason = "")
{
    if (on)
        WiFiSM.logEvent(F("OTGW Pump resume"));
    else
        WiFiSM.logEvent(F("OTGW Pump off: %s"), reason.c_str());

    otgwAttempt = 0;
    if (OTGW.setPump(on, reason) != HTTP_REQUEST_PENDING)
    {
        WiFiSM.logEvent(F("OTGW: %s"), OTGW.getLastError().c_str());
        return false;
    }
    return true;
}


void otgwPumpControl(bool pumpIsOn, int defrostingState)
{
    if ((defrostingState != 0) && !isDefrosting)
    {
        // Defrost started
        otgwSetPump(false, F("Defrost"));
    }
    else if ((defrostingState == 0) && isDefrosting)
    {
        // Defrost stopped
        otgwSetPump(true);
    }
    else if (pumpIsOn && (heatPumpOnCount == 0) && !isDefrosting && !antiFreezeActivated && !pumpPrime)
    {
        // Pump is on, but compressor not yet: "pump prime"
        pumpPrime = true;
        otgwSetPump(false, F("Prime"));
    }
    else if ((heatPumpOnCount == HP_ON_THRESHOLD) && pumpPrime)
    {
        // Compressor is on for 1 minute; pump prime finished
        pumpPrime = false;
        otgwSetPump(true);
    }
}


void antiFreezeControl(float inletTemp, float outletTemp, float compPower)
{
    int antiFreezeOnTemp = PersistentData.antiFreezeTemp;
    int antiFreezeOffTemp = antiFreezeOnTemp + ANTI_FREEZE_DELTA_T;

    if (!antiFreezeActivated)
    {
        // Anti-freeze control is not active (yet)
        if ((std::min(inletTemp, outletTemp) <= antiFreezeOnTemp) || testAntiFreeze)
        {
            antiFreezeActivated = true;
            WiFiSM.logEvent(F("Anti-freeze activated."));
            if (!HeatPump.setPump(true))
                WiFiSM.logEvent(F("Unable to activate pump."));
        }  
    }
    else
    {
        // Anti-freeze control is active
        if ((std::min(inletTemp, outletTemp) > antiFreezeOffTemp) && !testAntiFreeze)
        {
            antiFreezeActivated = false;
            WiFiSM.logEvent(F("Anti-freeze deactivated."));

            // Don't stop pump if compressor started in the meantime.
            if (compPower == 0)
            {
                if (!HeatPump.setPump(false))
                    WiFiSM.logEvent(F("Unable to deactivate pump."));
            }
            else
                WiFiSM.logEvent(F("Compressor is on."));
        }  
    }
}


void updateDayStats(uint32_t secondsSinceLastUpdate, float powerInKW, float powerOutKW, int defrostingState)
{
    if ((currentTime / SECONDS_PER_DAY) > (lastDayStatsEntryPtr->startTime / SECONDS_PER_DAY))
    {
        newDayStatsEntry();
        HeatPump.resetPacketStats();
    }

    lastDayStatsEntryPtr->update(currentTime, secondsSinceLastUpdate, powerInKW, powerOutKW, antiFreezeActivated);

    if (defrostingState != 0 && !isDefrosting)
        lastDayStatsEntryPtr->defrosts++; // Defrost started

    if (heatPumpOnCount == HP_ON_THRESHOLD)
        lastDayStatsEntryPtr->onCount++;
}


void updateTopicLog()
{
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        TopicId topicId = MonitoredTopics[i].id;
        newTopicLogEntry.topicValues[i] += HeatPump.getTopic(topicId).getValue().toFloat();
    }

    topicLogAggregations++;
    if (currentTime < topicLogAggregationTime) return; 

    // Calculate averages
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        newTopicLogEntry.topicValues[i] /= topicLogAggregations;

    if ((lastTopicLogEntryPtr == nullptr) || !newTopicLogEntry.equals(lastTopicLogEntryPtr))
    {
        lastTopicLogEntryPtr = TopicLog.add(&newTopicLogEntry);

        newTopicLogEntry.time = currentTime;

        ftpSyncEntries = std::min(ftpSyncEntries + 1, TOPIC_LOG_SIZE);
        if (PersistentData.ftpIsEnabled() && ftpSyncEntries == PersistentData.ftpSyncEntries)
            syncFTPTime = currentTime;
    }

    newTopicLogEntry.reset(); // Keep moving average of last minute only
    topicLogAggregations = 0;
    topicLogAggregationTime += AGGREGATION_INTERVAL;
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
        if (ftpSyncEntries > 0)
        {
            TopicLogEntry* firstEntryPtr = TopicLog.getEntryFromEnd(ftpSyncEntries);
            writeTopicLogCsv(firstEntryPtr, dataClient);
            ftpSyncEntries = 0;
        }
        else if (printTo != nullptr)
            printTo->println("Nothing to sync.");
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


void writeTopicLogCsv(TopicLogEntry* logEntryPtr, Print& destination)
{
    while (logEntryPtr != nullptr)
    {
        destination.print(formatTime("%F %H:%M", logEntryPtr->time));

        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            destination.print(";");
            destination.print(MonitoredTopics[i].formatValue(logEntryPtr->topicValues[i], false, 1));
        }
        destination.println();

        logEntryPtr = TopicLog.getNextEntry();
    }
}


void sendChunk(bool isLast = false)
{
    TRACE(F("Chunk size: %d\n"), HttpResponse.length());
    WebServer.sendContent(HttpResponse);
    HttpResponse.clear();
    if (isLast)
        WebServer.chunkedResponseFinalize();
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
    if (!PersistentData.ftpIsEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    const char* lastPacket = (lastPacketReceivedTime == 0)
        ? "Not yet"
        : formatTime("%H:%M:%S", lastPacketReceivedTime);

    WebServer.chunkedResponseModeStart(200, ContentTypeHtml);
    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("Aquarea"), HeatPump.getTopic(TopicId::Error).getValue());
    Html.writeRow(F("Last packet"), lastPacket);
    Html.writeRow(F("Packet errors"), F("%0.1f %%"), HeatPump.getPacketErrorRatio() * 100);
    Html.writeRow(F("FTP Sync"), ftpSync);
    Html.writeRow(F("Sync entries"), F("%d / %d"), ftpSyncEntries, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    sendChunk();

    if (lastPacketReceivedTime != 0)
        writeCurrentValues();
    
    sendChunk();

    writeStatisticsPerDay();

    Html.writeDivEnd();
    Html.writeFooter();

    sendChunk(true);
}


void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();
        float barValue = (topicValue - topic.minValue) / (topic.maxValue - topic.minValue);

        String barCssClass = topic.style;
        barCssClass += "Bar";

        Html.writeRowStart();
        Html.writeHeaderCell(FPSTR(topic.htmlLabel));
        Html.writeCell(topic.formatValue(topicValue, true));
        Html.writeCellStart(F("graph"));
        Html.writeBar(barValue, barCssClass, true);
        Html.writeCellEnd();
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void writeStatisticsPerDay()
{
    // Auto-ranging: determine max energy
    float maxEnergy = 1.0; // Prevent division by zero
    DayStatsEntry* dayStatsEntryPtr = DayStats.getFirstEntry();
    while (dayStatsEntryPtr != nullptr)
    {
        maxEnergy = std::max(maxEnergy, dayStatsEntryPtr->energyOut);
        dayStatsEntryPtr = DayStats.getNextEntry();
    }

    Html.writeSectionStart(F("Statistics per day"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Day"));
    Html.writeHeaderCell(F("Start"));
    Html.writeHeaderCell(F("Stop"));
    Html.writeHeaderCell(F("On time"));
    Html.writeHeaderCell(F("Avg on"));
    Html.writeHeaderCell(F("Runs"));
    Html.writeHeaderCell(F("&#10054;")); // Defrosts
    Html.writeHeaderCell(F("Anti-freeze"));
    Html.writeHeaderCell(F("E<sub>in</sub> (kWh)"));
    Html.writeHeaderCell(F("E<sub>out</sub> (kWh)"));
    Html.writeHeaderCell(F("COP"));
    Html.writeRowEnd();

    dayStatsEntryPtr = DayStats.getFirstEntry();
    while (dayStatsEntryPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%a", dayStatsEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", dayStatsEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", dayStatsEntryPtr->stopTime));
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->onSeconds));
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->getAvgOnSeconds()));
        Html.writeCell(dayStatsEntryPtr->onCount);
        Html.writeCell(dayStatsEntryPtr->defrosts);
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->antiFreezeSeconds, false));
        Html.writeCell(dayStatsEntryPtr->energyIn, F("%0.2f"));
        Html.writeCell(dayStatsEntryPtr->energyOut, F("%0.2f"));
        Html.writeCell(dayStatsEntryPtr->getCOP());

        Html.writeCellStart(F("graph"));
        Html.writeStackedBar(
            dayStatsEntryPtr->energyIn / maxEnergy,
            (dayStatsEntryPtr->energyOut - dayStatsEntryPtr->energyIn) / maxEnergy,
            F("inBar"),
            F("outBar"),
            false,
            false);
        Html.writeCellEnd();

        Html.writeRowEnd();

        dayStatsEntryPtr = DayStats.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpTopicsRequest()
{
    Tracer tracer(F("handleHttpTopicsRequest"));

    WebServer.chunkedResponseModeStart(200, ContentTypeHtml);
    Html.writeHeader(F("Topics"), Nav);

    if (lastPacketReceivedTime != 0)
    {
        Html.writeParagraph(
            F("Packet received @ %s"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        Html.writeTableStart();

        int i = 0;
        for (TopicId topicId : HeatPump.getAllTopicIds())
        {
            Topic topic = HeatPump.getTopic(topicId);

            Html.writeRowStart();
            Html.writeHeaderCell(topic.getName());
            Html.writeCell(topic.getValue());
            Html.writeCell(topic.getDescription());
            Html.writeRowEnd();

            if (i++ % 20 == 0)
                sendChunk();
        }

        Html.writeTableEnd();
    }

    Html.writeFooter();
    sendChunk(true);
}


void handleHttpTopicLogRequest()
{
    Tracer tracer(F("handleHttpTopicLogRequest"));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((TopicLog.count() - 1) / TOPIC_LOG_PAGE_SIZE) + 1;

    WebServer.chunkedResponseModeStart(200, ContentTypeHtml);

    Html.writeHeader(F("Aquarea log"), Nav);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        Html.writeHeaderCell(FPSTR(MonitoredTopics[i].htmlLabel));
    }
    Html.writeRowEnd();

    TopicLogEntry* logEntryPtr = TopicLog.getFirstEntry();
    for (int i = 0; i < (currentPage * TOPIC_LOG_PAGE_SIZE) && logEntryPtr != nullptr; i++)
    {
        logEntryPtr = TopicLog.getNextEntry();
    }
    for (int i = 0; i < TOPIC_LOG_PAGE_SIZE && logEntryPtr != nullptr; i++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        for (int k = 0; k < NUMBER_OF_MONITORED_TOPICS; k++)
        {
            Html.writeCell(MonitoredTopics[k].formatValue(logEntryPtr->topicValues[k], false, 1));
        }
        Html.writeRowEnd();

        if (i % 10 == 0)
            sendChunk();

        logEntryPtr = TopicLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    sendChunk(true);
}


void handleHttpHexDumpRequest()
{
    Tracer tracer(F("handleHttpHexDumpRequest"));

    if (WebServer.hasArg("raw"))
    {
        HttpResponse.clear();
        HeatPump.writeHexDump(HttpResponse, false);

        WebServer.send(200, ContentTypeText, HttpResponse);
    }
    else
    {
        Html.writeHeader(F("Hex dump"), Nav);

        Html.writeParagraph(
            F("Received %u valid packets, %u repaired packets, %u invalid packets."),
            HeatPump.getValidPackets(),
            HeatPump.getRepairedPackets(),
            HeatPump.getInvalidPackets());

        Html.writeParagraph(
            F("Last valid packet @ %s"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        Html.writeParagraph(
            F("Last error @ %s : %s"),
            formatTime("%H:%M:%S", lastPacketErrorTime),
            HeatPump.getLastError().c_str());

        if (lastPacketReceivedTime != 0)
        {
            Html.writeHeading(F("Last valid packet"), 2);

            Html.writePreStart();
            HeatPump.writeHexDump(HttpResponse, false);
            Html.writePreEnd();
        }

        Html.writeHeading(F("Last invalid packet"), 2);

        Html.writePreStart();
        HeatPump.writeHexDump(HttpResponse, true);
        Html.writePreEnd();

        Html.writeFooter();

        WebServer.send(200, ContentTypeHtml, HttpResponse);
    }
}


void handleHttpTestRequest()
{
    Tracer tracer(F("handleHttpTestRequest"));

    Html.writeHeader(F("Test"), Nav);

    if (WiFiSM.shouldPerformAction(F("antiFreeze")))
    {
        testAntiFreeze = !testAntiFreeze;
        const char* switchState = testAntiFreeze ? "on" : "off"; 
        Html.writeParagraph(
            F("Anti-freeze test: %s"),
            switchState);
    }

    if (WiFiSM.shouldPerformAction(F("defrost")))
    {
        testDefrost = !testDefrost;
        handleNewAquareaData();
        const char* switchState = testDefrost ? "on" : "off"; 
        Html.writeParagraph(
            F("Defrost test: %s"),
            switchState);
    }

    Html.writeActionLink(F("antiFreeze"), F("Test anti-freeze (switch)"), currentTime, ButtonClass);
    Html.writeActionLink(F("defrost"), F("Test defrost (switch)"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillDayStats")))
    {
        for (int i = 0; i < 6; i++)
        {
            float powerOutKW = float(i); 
            float powerInKW = powerOutKW / 4;
            lastDayStatsEntryPtr->update(currentTime + i * SECONDS_PER_DAY, 3600, powerInKW, powerOutKW, i % 2 == 0);
            newDayStatsEntry();
        }
        Html.writeParagraph(F("Day Stats filled."));
    }
    else
        Html.writeActionLink(F("fillDayStats"), F("Fill Day Stats"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillTopicLog")))
    {
        for (int i = 0; i < TOPIC_LOG_SIZE; i++)
        {
            newTopicLogEntry.time = currentTime + (i * 60);
            lastTopicLogEntryPtr = TopicLog.add(&newTopicLogEntry);
        }
        Html.writeParagraph(F("Topic Log filled."));
    }
    else
        Html.writeActionLink(F("fillTopicLog"), F("Fill Topic Log"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillEventLog")))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            WiFiSM.logEvent(F("Test event to fill the event log"));
        }
        Html.writeParagraph(F("Event Log filled."));
    }
    else
        Html.writeActionLink(F("fillEventLog"), F("Fill Event Log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpAquaMonJsonRequest()
{
    Tracer tracer(F("handleHttpAquaMonJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));

    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        String label = FPSTR(topic.label);
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();

        if (i > 0) HttpResponse.print(F(", "));
        HttpResponse.printf(
            F(" \"%s\": %s"),
            label.c_str(),
            topic.formatValue(topicValue, false));
    }

    HttpResponse.println(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F("handleHttpFtpSyncRequest"));

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

    Html.writeHeading(F("CSV header"), 2);
    Html.writePreStart();
    HttpResponse.print("Time");
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {

        HttpResponse.print(";");
        HttpResponse.print(FPSTR(MonitoredTopics[i].label));
    }
    Html.writePreEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);

}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    WebServer.chunkedResponseModeStart(200, ContentTypeHtml);
    Html.writeHeader(F("Event log"), Nav);

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    int i = 0;
    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv(F("%s"), event);
        event = EventLog.getNextEntry();
        if (i++ % 20 == 0)
            sendChunk();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();
    sendChunk(true);
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
    Html.writeNumberBox(CFG_FTP_SYNC_ENTRIES, F("FTP sync entries"), PersistentData.ftpSyncEntries, 0, TOPIC_LOG_SIZE);
    Html.writeNumberBox(CFG_ANTI_FREEZE_TEMP, F("T<sub>Anti-freeze</sub>"), PersistentData.antiFreezeTemp, 4, 10);
    Html.writeNumberBox(CFG_ZONE1_OFFSET, F("Zone1 offset"), PersistentData.zone1Offset, -5, 5, 1);
    Html.writeCheckbox(CFG_LOG_PACKET_ERRORS, F("Log packet errors"), PersistentData.logPacketErrors);
    Html.writeTextBox(CFG_OTGW_HOST, F("OTGW host"), PersistentData.otgwHost, sizeof(PersistentData.otgwHost) - 1);
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
    copyString(WebServer.arg(CFG_OTGW_HOST), PersistentData.otgwHost, sizeof(PersistentData.otgwHost)); 

    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_SYNC_ENTRIES).toInt();
    PersistentData.antiFreezeTemp = WebServer.arg(CFG_ANTI_FREEZE_TEMP).toInt();
    PersistentData.zone1Offset = WebServer.arg(CFG_ZONE1_OFFSET).toFloat();
    PersistentData.logPacketErrors = WebServer.arg(CFG_LOG_PACKET_ERRORS) == "true";

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    HeatPump.setZone1Offset(PersistentData.zone1Offset);

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
