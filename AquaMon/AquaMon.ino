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
#include "PersistentData.h"
#include "Aquarea.h"
#include "MonitoredTopics.h"
#include "DayStatsEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define TOPIC_LOG_SIZE 150
#define TOPIC_LOG_CSV_MAX_SIZE 100
#define DEFAULT_BAR_LENGTH 60
#define FTP_RETRY_INTERVAL (30 * 60)
#define QUERY_AQUAREA_INTERVAL 6
#define AGGREGATION_INTERVAL 60
#define ANTI_FREEZE_DELTA_T 5

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

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ContentTypeJson = "application/json";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(12288); // 12KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, DEFAULT_BAR_LENGTH);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TopicLogEntry> TopicLog(TOPIC_LOG_SIZE);
StaticLog<DayStatsEntry> DayStats(7);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Aquarea HeatPump;

TopicLogEntry newTopicLogEntry;
int topicLogAggregations = 0;

TopicLogEntry* lastTopicLogEntryPtr = nullptr;
DayStatsEntry* lastDayStatsEntryPtr = nullptr;

uint16_t ftpSyncEntries = 0;
uint32_t validPackets = 0;
uint32_t packetErrors = 0;
bool heatPumpIsOn = false;
bool antiFreezeActivated = false;

time_t currentTime = 0;
time_t queryAquareaTime = 0;
time_t lastPacketReceivedTime = 0;
time_t lastPacketErrorTime = 0;
time_t topicLogAggregationTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


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

    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/topics", handleHttpTopicsRequest);
    WebServer.on("/topiclog", handleHttpTopicLogRequest);
    WebServer.on("/hexdump", handleHttpHexDumpRequest);
    WebServer.on("/json", handleHttpHeishamonJsonRequest);
    WebServer.on("/json2", handleHttpAquaMonJsonRequest);
    WebServer.on("/sync", handleHttpFtpSyncRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    HeatPump.begin();

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
        {
            validPackets++;
            handleNewAquareaData();
        }
        else
        {
            lastPacketErrorTime = currentTime;
            packetErrors++;
            if (PersistentData.logPacketErrors)
                logEvent(HeatPump.getLastError());
        }
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if (currentTime >= queryAquareaTime)
    {
        queryAquareaTime += QUERY_AQUAREA_INTERVAL;
        if (!HeatPump.sendQuery())
            logEvent(F("Failed sending Aquarea query"));
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


void handleNewAquareaData()
{
    Tracer tracer(F("handleNewAquareaData"));

    uint32_t secondsSinceLastUpdate = (lastPacketReceivedTime == 0) ? 0 : (currentTime - lastPacketReceivedTime);
    lastPacketReceivedTime = currentTime;

    int inletTemp = HeatPump.getTopic(TopicId::Main_Inlet_Temp).getValue().toInt();
    int outletTemp = HeatPump.getTopic(TopicId::Main_Outlet_Temp).getValue().toInt();
    float compPower = HeatPump.getTopic(TopicId::Compressor_Power).getValue().toFloat();
    float heatPower = HeatPump.getTopic(TopicId::Heat_Power).getValue().toFloat();

    antiFreezeControl(inletTemp, outletTemp, compPower);
    updateDayStats(secondsSinceLastUpdate, compPower, heatPower);
    updateTopicLog();
}


void antiFreezeControl(int inletTemp, int outletTemp, float compPower)
{
    int antiFreezeOnTemp = PersistentData.antiFreezeTemp;
    int antiFreezeOffTemp = antiFreezeOnTemp + ANTI_FREEZE_DELTA_T;

    if (!antiFreezeActivated)
    {
        // Anti-freeze control is not active (yet)
        if (std::min(inletTemp, outletTemp) <= antiFreezeOnTemp)
        {
            antiFreezeActivated = true;
            logEvent(F("Anti-freeze activated."));
            if (!HeatPump.setPump(true))
                logEvent(F("Unable to activate pump."));
        }  
    }
    else
    {
        // Anti-freeze control is active
        if (std::min(inletTemp, outletTemp) > antiFreezeOffTemp)
        {
            antiFreezeActivated = false;
            logEvent(F("Anti-freeze deactivated."));

            // Don't stop pump if compressor started in the meantime.
            if (compPower == 0)
            {
                if (!HeatPump.setPump(false))
                    logEvent(F("Unable to deactivate pump."));
            }
            else
                logEvent(F("Compressor is on."));
        }  
    }
}


void updateDayStats(uint32_t secondsSinceLastUpdate, float powerInKW, float powerOutKW)
{
    TRACE(F("Pin: %0.2f kW, Pout: %0.2f kW, dt: %u s\n"), powerInKW, powerOutKW, secondsSinceLastUpdate);

    if ((currentTime / SECONDS_PER_DAY) > (lastDayStatsEntryPtr->startTime / SECONDS_PER_DAY))
    {
        newDayStatsEntry();
        validPackets = 0;
        packetErrors = 0;
        HeatPump.repairedPackets = 0;
    }

    lastDayStatsEntryPtr->update(currentTime, secondsSinceLastUpdate, powerInKW, powerOutKW, antiFreezeActivated);

    if ((powerInKW > 0) && !heatPumpIsOn)
        lastDayStatsEntryPtr->onCount++; // Heat pump was switched on
    heatPumpIsOn = powerInKW > 0;
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
        if (ftpSyncEntries == PersistentData.ftpSyncEntries)
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
        FTPClient.end();
        return false;
    }

    bool success = true;
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
            lastFTPSyncTime = currentTime;
        else
        {
            TRACE(F("FTP Append command failed: %s\n"), FTPClient.getLastResponse());
            success = false;
        }
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


void test(String message)
{
    Tracer tracer(F("test"), message.c_str());

    if (message.startsWith("L"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            logEvent(F("Test event to fill the event log"));
            yield();
        }
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

    String ftpSync;
    if (!PersistentData.ftpIsEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    const char* lastPacket = (lastPacketReceivedTime == 0)
        ? "Not received"
        : formatTime("%H:%M:%S", lastPacketReceivedTime);

    float packetErrorRatio = (packetErrors == 0) ? 0.0 : float(packetErrors) / (validPackets + packetErrors);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    Html.writeHeading(F("AquaMon status"));
    Html.writeTableStart();
    HttpResponse.printf(F("<tr><th>RSSI</th><td>%d dBm</td></tr>\r\n"), static_cast<int>(WiFi.RSSI()));
    HttpResponse.printf(F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    HttpResponse.printf(F("<tr><th><a href=\"/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSync.c_str());
    HttpResponse.printf(F("<tr><th>FTP Sync entries</th><td>%d</td></tr>\r\n"), ftpSyncEntries);
    HttpResponse.printf(F("<tr><th><a href=\"/topics\">Last packet</a></th><td>%s</td></tr>\r\n"), lastPacket);
    HttpResponse.printf(F("<tr><th>Packet errors</th><td>%0.0f %%</td></tr>\r\n"), packetErrorRatio * 100);
    HttpResponse.printf(F("<tr><th><a href=\"/topiclog\">Topic log</a></th><td>%d</td></p>\r\n"), TopicLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    Html.writeTableEnd();

    if (lastPacketReceivedTime != 0)
        writeCurrentValues();
    
    writeStatisticsPerDay();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeCurrentValues()
{
    Html.writeHeading(F("Current values"));

    Html.writeTableStart();
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();
        float barValue = (topicValue - topic.minValue) / (topic.maxValue - topic.minValue);

        String barCssClass = topic.style;
        barCssClass += "Bar";

        Html.writeRowStart();
        Html.writeHeaderCell(topic.htmlLabel);
        Html.writeCell(topic.formatValue(topicValue, true));
        Html.writeCellStart(F("graph"));
        Html.writeBar(barValue, barCssClass, true);
        Html.writeCellEnd();
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
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

    Html.writeHeading(F("Statistics per day"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Day"));
    Html.writeHeaderCell(F("Start"));
    Html.writeHeaderCell(F("Stop"));
    Html.writeHeaderCell(F("Anti-freeze"));
    Html.writeHeaderCell(F("On time"));
    Html.writeHeaderCell(F("Avg on time"));
    Html.writeHeaderCell(F("On count"));
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
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->antiFreezeSeconds));
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->onSeconds));
        Html.writeCell(formatTimeSpan(dayStatsEntryPtr->getAvgOnSeconds()));
        Html.writeCell(dayStatsEntryPtr->onCount);
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
}


void handleHttpTopicsRequest()
{
    Tracer tracer(F("handleHttpTopicsRequest"));

    Html.writeHeader(F("Topics"), true, true, HTTP_POLL_INTERVAL);

    if (lastPacketReceivedTime != 0)
    {
        HttpResponse.printf(
            F("<p>Packet received @ %s. <a href=\"/hexdump\">Show hex dump</a></p>\r\n"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        Html.writeTableStart();

        for (TopicId topicId : HeatPump.getAllTopicIds())
        {
            Topic topic = HeatPump.getTopic(topicId);

            Html.writeRowStart();
            Html.writeHeaderCell(topic.getName());
            Html.writeCell(topic.getValue());
            Html.writeCell(topic.getDescription());
            Html.writeRowEnd();
        }

        Html.writeTableEnd();
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpTopicLogRequest()
{
    Tracer tracer(F("handleHttpTopicLogRequest"));

    HttpResponse.clear();

    // Write CSV header
    HttpResponse.print("Time");
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        HttpResponse.print(";");
        HttpResponse.print(MonitoredTopics[i].label);
    }
    HttpResponse.println();

    // Write CSV data lines
    TopicLogEntry* firstEntryPtr = (TopicLog.count() <= TOPIC_LOG_CSV_MAX_SIZE)
        ? TopicLog.getFirstEntry()
        : TopicLog.getEntryFromEnd(TOPIC_LOG_CSV_MAX_SIZE);
    writeTopicLogCsv(TopicLog.getFirstEntry(), HttpResponse);

    WebServer.send(200, ContentTypeText, HttpResponse);
}


void handleHttpHexDumpRequest()
{
    Tracer tracer(F("handleHttpHexDumpRequest"));

    Html.writeHeader(F("Hex dump"), true, true);

    HttpResponse.printf(
        F("<p>Received %u valid packets, %u repaired packets, %u invalid packets.</p>\r\n"),
        validPackets,
        HeatPump.repairedPackets,
        packetErrors);

    if (lastPacketReceivedTime != 0)
    {
        Html.writeHeading(F("Last valid packet"), 2);
        HttpResponse.printf(
            F("<p>Packet received @ %s</p>\r\n"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        HttpResponse.println(F("<div class=\"hexdump\"><pre>"));
        HeatPump.writeHexDump(HttpResponse, false);
        HttpResponse.println(F("</pre></div>"));
    }

    Html.writeHeading(F("Last invalid packet"), 2);
    HttpResponse.printf(
        F("<p>Last error @ %s : %s</p>\r\n"),
        formatTime("%H:%M:%S", lastPacketErrorTime),
        HeatPump.getLastError().c_str());

    HttpResponse.println(F("<div class=\"hexdump\"><pre>"));
    HeatPump.writeHexDump(HttpResponse, true);
    HttpResponse.println(F("</pre></div>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpHeishamonJsonRequest()
{
    Tracer tracer(F("handleHttpHeishamonJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ \"heatpump\": [ "));

    int i = 0;
    for (TopicId topicId : HeatPump.getAllTopicIds())
    {
        Topic topic = HeatPump.getTopic(topicId);
        if (i++ > 0) HttpResponse.print(F(", "));
        HttpResponse.printf(
            F("{ \"Topic\": \"%s\", \"Name\": \"%s\", \"Value\": \"%s\", \"Description\": \"%s\" }"),
            topic.getId().c_str(),
            topic.getName().c_str(),
            topic.getValue().c_str(),
            topic.getDescription().c_str());
    }

    HttpResponse.println(F(" ] }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}



void handleHttpAquaMonJsonRequest()
{
    Tracer tracer(F("handleHttpAquaMonJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));

    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();

        if (i > 0) HttpResponse.print(F(", "));
        HttpResponse.printf(
            F(" \"%s\": %s"),
            topic.label,
            topic.formatValue(topicValue, false));
    }

    HttpResponse.println(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F("handleHttpFtpSyncRequest"));

    Html.writeHeader(F("FTP Sync"), true, true);

    HttpResponse.println(F("<div><pre>"));
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println(F("</pre></div>"));

    if (success)
    {
        HttpResponse.println(F("<p>Success!</p>"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        HttpResponse.println(F("<p>Failed!</p>"));
 
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);

}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    Html.writeHeader(F("Event log"), true, true);

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        logEvent(F("Event log cleared."));
    }
    else
        HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear event log</a></p>\r\n"), currentTime);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Configuration"), true, true);

    Html.writeFormStart(F("/config"));
    Html.writeTableStart();
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1);
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1);
    Html.writeTextBox(CFG_FTP_SYNC_ENTRIES, F("FTP sync entries"), String(PersistentData.ftpSyncEntries), 3);
    Html.writeTextBox(CFG_ANTI_FREEZE_TEMP, F("Anti-freeze temperature"), String(PersistentData.antiFreezeTemp), 2);
    Html.writeCheckbox(CFG_LOG_PACKET_ERRORS, F("Log packet errors"), PersistentData.logPacketErrors);
    Html.writeTableEnd();
    Html.writeSubmitButton();
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
        WiFiSM.reset();
    else
        HttpResponse.printf(F("<p><a href=\"?reset=%u\">Reset ESP</a></p>\r\n"), currentTime);

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

    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_SYNC_ENTRIES).toInt();
    PersistentData.antiFreezeTemp = WebServer.arg(CFG_ANTI_FREEZE_TEMP).toInt();
    PersistentData.logPacketErrors = WebServer.arg(CFG_LOG_PACKET_ERRORS) == "true";

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
