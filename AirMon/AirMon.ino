#define DEBUG_ESP_PORT Serial

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Ticker.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <Wire.h>
#include <bsec.h>
#include "PersistentData.h"
#include "MonitoredTopics.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define DISPLAY_INTERVAL 2.0F
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (30 * 60)
#define HOUR_LOG_INTERVAL (30 * 60)
#define IAQ_LOG_SIZE 250
#define IAQ_LOG_PAGE_SIZE 50
#define IAQ_SENSOR_COUNT 6

#define FAN_PIN 25

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_FTP_ENTRIES F("FTPEntries")
#define CFG_FAN_THRESHOLD F("FanThreshold")
#define CFG_FAN_HYSTERESIS F("FanHysteresis")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TopicLogEntry> IAQLog(IAQ_LOG_SIZE);
StaticLog<TopicLogEntry> HourStats(24 * 2); // 24 hrs
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Adafruit_SSD1306 Display(128, 64, &Wire);
Ticker DisplayTicker;

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

bool fanIsOn = false;
int ftpSyncEntries = 0;
int showTopic = 0;
TopicLogEntry current;

TopicLogEntry* lastLogEntryPtr = nullptr;
TopicLogEntry* lastStatsEntryPtr = nullptr;

Bsec IAQSensor;
bsec_virtual_sensor_t iaqSensorIds[IAQ_SENSOR_COUNT] =
{
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_STATIC_IAQ,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY
};
bsec_library_return_t lastBsecStatus = BSEC_OK;
int8_t lastBme680Status = BME680_OK;


// Boot code
void setup() 
{
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    Serial.begin(115200); // Use same baudrate as bootloader
    Serial.setTimeout(1000);
    Serial.println();

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
    WebServer.on("/json", handleHttpJsonRequest);
    WebServer.on("/iaqlog", handleHttpIAQLogRequest);
    WebServer.on("/sync", handleHttpFtpSyncRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerInitializing, onWiFiConnected);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    pinMode(FAN_PIN, OUTPUT);
    setFanState(false);

    if (Wire.begin(5, 4))
    {
        if (Display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
            displayMessage(F("Booting..."));
        else
            WiFiSM.logEvent(F("Display initialization failed"));

        initializeIAQSensor();
    }
    else
        WiFiSM.logEvent(F("Failed initializing I2C"));

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


void displayMessage(String text)
{
    Tracer tracer(F(__func__), text.c_str());

    Display.clearDisplay();
    Display.setTextColor(SSD1306_WHITE);
    Display.cp437(true);

    Display.setTextSize(2, 4);
    Display.setCursor(0, 0);
    Display.print(PersistentData.hostName);

    if (text != nullptr)
    {
        Display.setTextSize(1, 2);
        Display.setCursor(0, 40);
        Display.print(text.c_str());
    }

    Display.display();
}


void displayTopic()
{
    Tracer tracer(F(__func__), MonitoredTopics[showTopic].label);

    const char* value = MonitoredTopics[showTopic].formatValue(current.topicValues[showTopic], false);
    const char* unitOfMeasure = (showTopic == TopicId::Temperature) ? "\xF8" "C" : MonitoredTopics[showTopic].unitOfMeasure;

    Display.clearDisplay();
    Display.setTextColor(SSD1306_WHITE);

    Display.setTextSize(1, 2);
    Display.setCursor(0, 0);
    Display.print(MonitoredTopics[showTopic].label);

    Display.setTextSize(3, 6);
    Display.setCursor(0, 16);
    Display.print(value);

    int16_t x1, y1;
    uint16_t w, h;
    Display.getTextBounds(value, 0, 8, &x1, &y1, &w, &h);
    Display.setCursor(w + 2, 18);
    Display.setTextSize(2, 4);
    Display.print(unitOfMeasure);

    Display.display();

    showTopic = (showTopic + 1) % (NUMBER_OF_MONITORED_TOPICS - 2);
}


bool initializeIAQSensor()
{
    IAQSensor.begin(BME680_I2C_ADDR_PRIMARY, Wire);
    if (!checkIAQStatus()) return false;

    bsec_version_t v = IAQSensor.version;
    WiFiSM.logEvent("BSEC v%d.%d.%d.%d", v.major, v.minor, v.major_bugfix, v.minor_bugfix);

    IAQSensor.updateSubscription(iaqSensorIds, IAQ_SENSOR_COUNT, BSEC_SAMPLE_RATE_LP);
    if (IAQSensor.status != BSEC_OK)
    {
        WiFiSM.logEvent(F("Failed BSEC subscription: %d"), IAQSensor.status);
        lastBsecStatus = IAQSensor.status;
       return false;
    }

    return true;
}


bool checkIAQStatus()
{
    bool result = true;
    if (IAQSensor.status != BSEC_OK)
    {
        if (IAQSensor.status != lastBsecStatus)
            WiFiSM.logEvent(F("BSEC status: %d"), IAQSensor.status);
        result = false;
    }
    else if (IAQSensor.bme680Status != BME680_OK) 
    {
        if (IAQSensor.bme680Status != lastBme680Status)
            WiFiSM.logEvent(F("BME680 status: %d"), IAQSensor.bme680Status);
        result = false;
    }

    lastBsecStatus = IAQSensor.status;
    lastBme680Status = IAQSensor.bme680Status;

    return result;
}


void setFanState(bool on)
{
    fanIsOn = on;
    digitalWrite(FAN_PIN, on ? 1 : 0);
    WiFiSM.logEvent(F("Fan switched %s"), on ? "on" : "off");
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();

    if (Serial.available())
    {
        String message = Serial.readStringUntil('\n');
        test(message);
    }
}


void onWiFiConnected()
{
    displayMessage(WiFiSM.getIPAddress());
}


void onWiFiTimeSynced()
{
    displayMessage(formatTime("%F %H:%M", currentTime));
    DisplayTicker.attach(DISPLAY_INTERVAL, displayTopic);
}


void onWiFiInitialized()
{
    if (IAQSensor.run())
        updateIAQ();
    else        
        checkIAQStatus();

    if (!WiFiSM.isConnected())
    {
        // WiFi connection is lost. 
        // Skip stuff that requires a WiFi connection.
        return;
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent(F("FTP synchronized"));
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastResponse());
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void updateIAQ()
{
    Tracer tracer(F(__func__));

    if (fanIsOn)
    {
        if (IAQSensor.staticIaq < (PersistentData.fanIAQThreshold - PersistentData.fanIAQHysteresis))
            setFanState(false);
    }
    else if (IAQSensor.staticIaq >= PersistentData.fanIAQThreshold)
        setFanState(true);

    current.topicValues[TopicId::Temperature] = IAQSensor.temperature;
    current.topicValues[TopicId::Pressure] = IAQSensor.pressure;
    current.topicValues[TopicId::Humidity] = IAQSensor.humidity;
    current.topicValues[TopicId::Accuracy] = IAQSensor.iaqAccuracy;
    current.topicValues[TopicId::IAQ] = IAQSensor.staticIaq;
    current.topicValues[TopicId::CO2Equivalent] = IAQSensor.co2Equivalent;
    current.topicValues[TopicId::BVOCEquivalent] = IAQSensor.breathVocEquivalent;
    current.topicValues[TopicId::Fan] = fanIsOn ? 1.0F : 0.0F;

    if (lastLogEntryPtr == nullptr || !current.equals(lastLogEntryPtr))
    {
        current.time = currentTime;
        lastLogEntryPtr = IAQLog.add(&current);
    }

    if (lastStatsEntryPtr == nullptr || currentTime >= (lastStatsEntryPtr->time + HOUR_LOG_INTERVAL))
    {
        TopicLogEntry statsEntry;
        statsEntry.time = currentTime - currentTime % HOUR_LOG_INTERVAL;
        statsEntry.reset();
        lastStatsEntryPtr = HourStats.add(&statsEntry);
    }
    lastStatsEntryPtr->aggregate(&current);
}


void writeIAQLogCsv(TopicLogEntry* logEntryPtr, Print& destination)
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

        logEntryPtr = IAQLog.getNextEntry();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F(__func__));

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
            TopicLogEntry* firstLogEntryPtr = IAQLog.getEntryFromEnd(ftpSyncEntries);
            writeIAQLogCsv(firstLogEntryPtr, dataClient);
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


void test(String message)
{
    Tracer tracer(F(__func__), message.c_str());

    if (message.startsWith("testL"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            WiFiSM.logEvent(F("Test event to fill the event log"));
            yield();
        }
    }
    else if (message.startsWith("testI"))
    {
        for (int i = 0; i < IAQ_LOG_SIZE; i++)
        {
            current.time = currentTime + i * 60;
            current.topicValues[TopicId::Temperature] = i % 30;
            current.topicValues[TopicId::Pressure] = i + 900;
            current.topicValues[TopicId::Humidity] = i % 100;
            current.topicValues[TopicId::Accuracy] = i % 4;
            current.topicValues[TopicId::IAQ] = (i * 5) % 500;
            current.topicValues[TopicId::CO2Equivalent] = i * 10;
            current.topicValues[TopicId::BVOCEquivalent] = i * 5;
            current.topicValues[TopicId::Fan] = (i % 2 == 0) ? 0.0F : 1.0F;
            lastLogEntryPtr = IAQLog.add(&current);
        }
    }
    else if (message.startsWith("testH"))
    {
        for (int i = 0; i < 48; i++)
        {
            TopicLogEntry statsEntry;
            statsEntry.time = currentTime + i * 1800;
            statsEntry.count = 1;
            statsEntry.topicValues[TopicId::Temperature] = i % 30;
            statsEntry.topicValues[TopicId::IAQ] = i * 5;
            statsEntry.topicValues[TopicId::Fan] = float(i) / 48;
            lastStatsEntryPtr = HourStats.add(&statsEntry);
        }
    }
    else if (message.startsWith("testR"))
    {
        WiFiSM.reset();
    }
}


const char* getIAQLevel(float iaq)
{
    if (iaq <= 50) return "Excellent";
    if (iaq <= 100) return "Good";
    if (iaq <= 150) return "LightlyPolluted";
    if (iaq <= 200) return "ModeratelyPolluted";
    return "HeavilyPolluted";
}


void handleHttpRootRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    const char* ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    Html.writeHeading(F("Air Monitor status"));

    Html.writeTableStart();
    HttpResponse.printf(F("<tr><th>RSSI</th><td>%d dBm</td></tr>\r\n"), static_cast<int>(WiFi.RSSI()));
    HttpResponse.printf(F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSync);
    if (PersistentData.isFTPEnabled())
        HttpResponse.printf(F("<tr><th>FTP Sync entries</th><td>%d / %d</td></tr>\r\n"), ftpSyncEntries, PersistentData.ftpSyncEntries);
    HttpResponse.printf(F("<tr><th><a href=\"/iaqlog\">IAQ log</a></th><td>%d</td></p>\r\n"), IAQLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    Html.writeTableEnd();

    writeCurrentValues();
    writeHourStats();

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
        float topicValue = current.topicValues[i];
        float barValue = (topicValue - topic.minValue) / (topic.maxValue - topic.minValue);

        String barCssClass = topic.style;
        if (i == TopicId::IAQ) barCssClass += getIAQLevel(topicValue);
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


void writeHourStats()
{
    Html.writeHeading(F("Last 24 hours"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    Html.writeHeaderCell(F("T (°C)"));
    Html.writeHeaderCell(F("Fan (%)"));
    Html.writeHeaderCell(F("IAQ"));
    Html.writeRowEnd();

    float minIAQ, maxIAQ;
    getIAQRange(minIAQ, maxIAQ);

    TopicLogEntry* statsEntryPtr = HourStats.getFirstEntry();
    while (statsEntryPtr != nullptr)
    {
        float t = statsEntryPtr->getAverage(TopicId::Temperature);
        float fan = statsEntryPtr->getAverage(TopicId::Fan) * 100;
        float iaq = statsEntryPtr->getAverage(TopicId::IAQ);

        float barValue = (iaq - minIAQ) / (maxIAQ - minIAQ);

        String barClass = F("iaq");
        barClass += getIAQLevel(iaq);
        barClass += F("Bar");

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", statsEntryPtr->time));
        Html.writeCell(t);
        Html.writeCell(fan, F("%0.0f"));
        Html.writeCell(iaq, F("%0.0f"));
        Html.writeCellStart(F("graph"));
        Html.writeBar(barValue, barClass, false);
        Html.writeCellEnd();
        Html.writeRowEnd();

        statsEntryPtr = HourStats.getNextEntry();
    }

    Html.writeTableEnd();
}


void getIAQRange(float& min, float& max)
{
    min = 666;
    max = 0;

    TopicLogEntry* statsEntryPtr = HourStats.getFirstEntry();
    while (statsEntryPtr != nullptr)
    {
        float iaq = statsEntryPtr->getAverage(TopicId::IAQ);
        min = std::min(min, iaq);
        max = std::max(max, iaq);
        statsEntryPtr = HourStats.getNextEntry();
    }

    if (max == min) max += 1.0F; // Prevent division by zero
}


void handleHttpJsonRequest()
{
    Tracer tracer(F(__func__));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));

    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = current.topicValues[i];

        if (i > 0) HttpResponse.print(F(", "));
        HttpResponse.printf(
            F(" \"%s\": %s"),
            topic.label,
            topic.formatValue(topicValue, false));
    }

    HttpResponse.println(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void handleHttpIAQLogRequest()
{
    Tracer tracer(F(__func__));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((IAQLog.count() - 1) / IAQ_LOG_PAGE_SIZE) + 1;

    Html.writeHeader(F("IAQ log"), true, true);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        Html.writeHeaderCell(MonitoredTopics[i].htmlLabel);
    }
    Html.writeRowEnd();

    TopicLogEntry* logEntryPtr = IAQLog.getFirstEntry();
    for (int i = 0; i < (currentPage * IAQ_LOG_PAGE_SIZE) && logEntryPtr != nullptr; i++)
    {
        logEntryPtr = IAQLog.getNextEntry();
    }
    for (int i = 0; i < IAQ_LOG_PAGE_SIZE && logEntryPtr != nullptr; i++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M:%S", logEntryPtr->time));
        for (int k = 0; k < NUMBER_OF_MONITORED_TOPICS; k++)
        {
            Html.writeCell(MonitoredTopics[k].formatValue(logEntryPtr->topicValues[k], false, 1));
        }
        Html.writeRowEnd();

        logEntryPtr = IAQLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F(__func__));

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
 
    Html.writeHeading(F("CSV header"), 2);
    HttpResponse.print("<div><pre>Time");
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        HttpResponse.print(";");
        HttpResponse.print(MonitoredTopics[i].label);
    }
    HttpResponse.println(F("</pre></div>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), true, true);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear event log</a></p>\r\n"), currentTime);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

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
    Html.writeTextBox(CFG_FTP_ENTRIES, F("FTP sync entries"), String(PersistentData.ftpSyncEntries), 3);
    Html.writeTextBox(CFG_FAN_THRESHOLD, F("Fan threshold"), String(PersistentData.fanIAQThreshold), 3);
    Html.writeTextBox(CFG_FAN_HYSTERESIS, F("Fan hysteresis"), String(PersistentData.fanIAQHysteresis), 3);
    Html.writeTableEnd();
    Html.writeSubmitButton();
    Html.writeFormEnd();

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
    Tracer tracer(F(__func__));

    copyString(WebServer.arg(CFG_WIFI_SSID), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID)); 
    copyString(WebServer.arg(CFG_WIFI_KEY), PersistentData.wifiKey, sizeof(PersistentData.wifiKey)); 
    copyString(WebServer.arg(CFG_HOST_NAME), PersistentData.hostName, sizeof(PersistentData.hostName)); 
    copyString(WebServer.arg(CFG_NTP_SERVER), PersistentData.ntpServer, sizeof(PersistentData.ntpServer)); 
    copyString(WebServer.arg(CFG_FTP_SERVER), PersistentData.ftpServer, sizeof(PersistentData.ftpServer)); 
    copyString(WebServer.arg(CFG_FTP_USER), PersistentData.ftpUser, sizeof(PersistentData.ftpUser)); 
    copyString(WebServer.arg(CFG_FTP_PASSWORD), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword)); 

    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_ENTRIES).toInt();
    PersistentData.fanIAQThreshold = WebServer.arg(CFG_FAN_THRESHOLD).toInt();
    PersistentData.fanIAQHysteresis = WebServer.arg(CFG_FAN_HYSTERESIS).toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
