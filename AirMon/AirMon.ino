//#define DEBUG_ESP_PORT Serial

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
#include <Navigation.h>
#include <Log.h>
#include <Wire.h>
#include <bsec.h>
#include <s8_uart.h>
#include "PersistentData.h"
#include "MonitoredTopics.h"

#define SECONDS_PER_DAY (24 * 3600)
#define BSEC_STATE_SAVE_INTERVAL (24 * 3600)
#define AGGREGATION_INTERVAL 60
#define HTTP_POLL_INTERVAL 60
#define DISPLAY_INTERVAL 2.0F
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (30 * 60)
#define HOUR_LOG_INTERVAL (30 * 60)
#define IAQ_LOG_SIZE 250
#define IAQ_LOG_PAGE_SIZE 50
#define BME_SENSOR_COUNT 3

#define FAN_PIN 14
#define CO2SENSOR_TXD_PIN 15
#define CO2SENSOR_RXD_PIN 13

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
#define CFG_TEMP_OFFSET F("TempOffset")
#define CFG_FAN_OFF_FROM F("FanOffFrom")
#define CFG_FAN_OFF_TO F("FanOffTo")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
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
    "Graph.svg",
    "Home.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], 60);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TopicLogEntry> IAQLog(IAQ_LOG_SIZE);
StaticLog<TopicLogEntry> HourStats(24 * 2); // 24 hrs
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Adafruit_SSD1306 Display(128, 64, &Wire);
Ticker DisplayTicker;
Ticker MeasureTicker;
S8_UART CO2Sensor(Serial1);
Navigation Nav;

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

bool fanIsOn = false;
bool isNightMode = false;
int ftpSyncEntries = 0;
int showTopic = 0;

float currentTopicValues[NUMBER_OF_MONITORED_TOPICS];
TopicLogEntry newLogEntry;

TopicLogEntry* lastLogEntryPtr = nullptr;
TopicLogEntry* lastStatsEntryPtr = nullptr;

Bsec IAQSensor;
const uint8_t iaqSensorConfig[] =
{
    0,8,4,1,61,0,0,0,0,0,0,0,174,1,0,0,48,0,1,0,0,192,168,71,64,49,119,76,0,0,225,68,137,65,0,191,205,204,204,190,0,0,64,191,225,122,148,190,0,0,0,0,216,85,0,100,0,0,0,0,0,0,0,0,28,0,2,0,0,244,1,225,0,25,0,0,128,64,0,0,32,65,144,1,0,0,112,65,0,0,0,63,16,0,3,0,10,215,163,60,10,215,35,59,10,215,35,59,9,0,5,0,0,0,0,0,1,88,0,9,0,229,208,34,62,0,0,0,0,0,0,0,0,218,27,156,62,225,11,67,64,0,0,160,64,0,0,0,0,0,0,0,0,94,75,72,189,93,254,159,64,66,62,160,191,0,0,0,0,0,0,0,0,33,31,180,190,138,176,97,64,65,241,99,190,0,0,0,0,0,0,0,0,167,121,71,61,165,189,41,192,184,30,189,64,12,0,10,0,0,0,0,0,0,0,0,0,229,0,254,0,2,1,5,48,117,100,0,44,1,112,23,151,7,132,3,197,0,92,4,144,1,64,1,64,1,144,1,48,117,48,117,48,117,48,117,100,0,100,0,100,0,48,117,48,117,48,117,100,0,100,0,48,117,48,117,100,0,100,0,100,0,100,0,48,117,48,117,48,117,100,0,100,0,100,0,48,117,48,117,100,0,100,0,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,44,1,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,8,7,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,112,23,255,255,255,255,255,255,255,255,220,5,220,5,220,5,255,255,255,255,255,255,220,5,220,5,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,44,1,0,0,0,0,237,52,0,0
};
bsec_virtual_sensor_t iaqSensorIds[BME_SENSOR_COUNT] =
{
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY
};
int8_t lastBme680Status = BME680_OK;
int16_t lastS8Status = 0;


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
            .label = PSTR("IAQ log"),
            .urlPath = PSTR("iaqlog"),
            .handler = handleHttpIAQLogRequest
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

    initializeCO2Sensor();

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

    String value = MonitoredTopics[showTopic].formatValue(currentTopicValues[showTopic], false);
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

    showTopic = (showTopic + 1) % (NUMBER_OF_MONITORED_TOPICS - 1);
}


bool initializeIAQSensor()
{
    IAQSensor.begin(BME680_I2C_ADDR_PRIMARY, Wire);
    if (!checkIAQStatus()) return false;

    bsec_version_t v = IAQSensor.version;
    WiFiSM.logEvent("BSEC v%d.%d.%d.%d", v.major, v.minor, v.major_bugfix, v.minor_bugfix);

    IAQSensor.setConfig(iaqSensorConfig);
    if (IAQSensor.status != BSEC_OK)
    {
        WiFiSM.logEvent(F("Failed setting BSEC config: %d"), IAQSensor.status);
        return false;
    }

    IAQSensor.setTemperatureOffset(PersistentData.tOffset);

    return true;
}


bool checkIAQStatus()
{
    bool result = true;
    if (IAQSensor.status != BSEC_OK)
    {
        WiFiSM.logEvent(F("BSEC status: %d"), IAQSensor.status);
        result = false;
    }
    else if (IAQSensor.bme680Status != BME680_OK) 
    {
        if (IAQSensor.bme680Status != lastBme680Status)
            WiFiSM.logEvent(F("BME680 status: %d"), IAQSensor.bme680Status);
        result = false;
    }

    lastBme680Status = IAQSensor.bme680Status;

    return result;
}


bool initializeCO2Sensor()
{
    Serial1.begin(S8_BAUDRATE, SERIAL_8N1, CO2SENSOR_RXD_PIN, CO2SENSOR_TXD_PIN);
    Serial1.setTimeout(500);

    char firmwareVersion[16];
    CO2Sensor.get_firmware_version(firmwareVersion);
    if (strlen(firmwareVersion) == 0)
    {
        WiFiSM.logEvent(F("CO2 sensor not found."));
        return false;
    }
    
    WiFiSM.logEvent(F("CO2 sensor version %s"), firmwareVersion);
    return true;
}


bool checkCO2SensorStatus()
{
    int16_t status = CO2Sensor.get_meter_status();

    if ((status != 0) && (status != lastS8Status))
        WiFiSM.logEvent(F("CO2 sensor error: 0x%02x"), status);

    lastS8Status = status;
    return (status == 0);
}


void setNightMode(bool on)
{
    Tracer tracer(F(__func__), on ? "on" : "off");

    isNightMode = on;

    int measureInterval;
    float bsecSampleRate;
    if (isNightMode)
    {
        measureInterval = 300;
        bsecSampleRate = BSEC_SAMPLE_RATE_ULP;
        DisplayTicker.detach();
        Display.clearDisplay();
        Display.display();
    }
    else
    {
        measureInterval = 6;
        bsecSampleRate = BSEC_SAMPLE_RATE_LP;
        DisplayTicker.attach(DISPLAY_INTERVAL, displayTopic);
    }

    IAQSensor.updateSubscription(iaqSensorIds, BME_SENSOR_COUNT, bsecSampleRate);
    if (IAQSensor.status != BSEC_OK)
        WiFiSM.logEvent(F("Failed BSEC subscription: %d"), IAQSensor.status);

    MeasureTicker.attach(measureInterval, updateIAQ);
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
    newLogEntry.time = currentTime;
    newLogEntry.reset();
    displayMessage(formatTime("%F %H:%M", currentTime));
    setNightMode(false);
}


void onWiFiInitialized()
{
    if (WiFiSM.isConnected())
    {
        if (isNightMode) setNightMode(false);
    }
    else
    {
        // WiFi connection is lost.
        if (!isNightMode) setNightMode(true);

        // Skip stuff that requires a WiFi connection.
        return;
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
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError().c_str());
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void updateIAQ()
{
    Tracer tracer(F(__func__));

    if (checkIAQStatus() && IAQSensor.run())
    {
        currentTopicValues[TopicId::Temperature] = IAQSensor.temperature;
        currentTopicValues[TopicId::Pressure] = IAQSensor.pressure / 100; // hPa
        currentTopicValues[TopicId::Humidity] = IAQSensor.humidity;
    }

    if (checkCO2SensorStatus())
        currentTopicValues[TopicId::CO2] = CO2Sensor.get_co2();

    if (currentTime >= newLogEntry.time + AGGREGATION_INTERVAL)
    {
        time_t startOfDay = getStartOfDay(currentTime);
        if (currentTime < startOfDay + PersistentData.fanOffToMinutes * 60
            || currentTime > startOfDay + PersistentData.fanOffFromMinutes * 60)
        {
            // Fan off time
            if (fanIsOn) setFanState(false);
        }
        else
        {
            // Switch fan on/off based on CO2 levels
            float avgCO2 = newLogEntry.getAverage(TopicId::CO2);
            if (fanIsOn)
            {
                if (avgCO2 < (PersistentData.fanCO2Threshold - PersistentData.fanCO2Hysteresis))
                    setFanState(false);
            }
            else if (avgCO2 >= PersistentData.fanCO2Threshold)
                setFanState(true);
        }

        if (lastLogEntryPtr == nullptr || !newLogEntry.equals(lastLogEntryPtr))
        {
            lastLogEntryPtr = IAQLog.add(&newLogEntry);
            ftpSyncEntries = std::min(ftpSyncEntries + 1, IAQ_LOG_SIZE);
            if (PersistentData.isFTPEnabled() && ftpSyncEntries >= PersistentData.ftpSyncEntries)
                syncFTPTime = currentTime;
        }
        newLogEntry.reset(); // Moving average
        newLogEntry.time = currentTime;
    }

    currentTopicValues[TopicId::Fan] = fanIsOn ? 1.0F : 0.0F;
    newLogEntry.aggregate(currentTopicValues);

    if (lastStatsEntryPtr == nullptr || currentTime >= (lastStatsEntryPtr->time + HOUR_LOG_INTERVAL))
    {
        TopicLogEntry statsEntry;
        statsEntry.time = currentTime - currentTime % HOUR_LOG_INTERVAL;
        statsEntry.reset();
        lastStatsEntryPtr = HourStats.add(&statsEntry);
    }
    lastStatsEntryPtr->aggregate(currentTopicValues);
}


void writeIAQLogCsv(TopicLogEntry* logEntryPtr, Print& destination)
{
    while (logEntryPtr != nullptr)
    {
        destination.print(formatTime("%F %H:%M", logEntryPtr->time));

        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            destination.print(";");
            destination.print(MonitoredTopics[i].formatValue(logEntryPtr->getAverage(i), false, 1));
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
            printTo->println(F("Nothing to sync."));
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
            lastFTPSyncTime = currentTime;
        else
        {
            FTPClient.setUnexpectedResponse();
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
            currentTopicValues[TopicId::Temperature] = i % 30;
            currentTopicValues[TopicId::Pressure] = i + 900;
            currentTopicValues[TopicId::Humidity] = i % 100;
            currentTopicValues[TopicId::CO2] = i * 10;
            currentTopicValues[TopicId::Fan] = (i % 2 == 0) ? 0.0F : 1.0F;

            TopicLogEntry testLogEntry;
            testLogEntry.time = currentTime + i * 60;
            testLogEntry.reset();
            testLogEntry.aggregate(currentTopicValues);
            lastLogEntryPtr = IAQLog.add(&testLogEntry);
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
            statsEntry.topicValues[TopicId::CO2] = 400 + i * 10;
            statsEntry.topicValues[TopicId::Fan] = float(i) / 48;
            lastStatsEntryPtr = HourStats.add(&statsEntry);
        }
    }
    else if (message.startsWith("testN"))
    {
        bool on = message.substring(5).toInt();
        setNightMode(on);
    }
}


const char* getIAQLevel(float co2)
{
    if (co2 <= 750) return "Excellent";
    if (co2 <= 1000) return "Good";
    if (co2 <= 1250) return "LightlyPolluted";
    if (co2 <= 1500) return "ModeratelyPolluted";
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

    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("FTP Sync"), ftpSync);
    if (PersistentData.isFTPEnabled())
        Html.writeRow(F("Sync entries"), F("%d / %d"), ftpSyncEntries, PersistentData.ftpSyncEntries);        
    Html.writeRow(
        F("Fan on"),
        F("<div>%s</div><div>%d CO<sub>2</sub></div>"),
        formatTime("%H:%M", getStartOfDay(currentTime) + PersistentData.fanOffToMinutes * 60),
        PersistentData.fanCO2Threshold);
    Html.writeRow(
        F("Fan off"),
        F("<div>%s</div><div>%d CO<sub>2</sub></div>"),
        formatTime("%H:%M", getStartOfDay(currentTime) + PersistentData.fanOffFromMinutes * 60),
        PersistentData.fanCO2Threshold - PersistentData.fanCO2Hysteresis);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeCurrentValues();
    writeHourStats();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();

    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = currentTopicValues[i];
        float barValue = (topicValue - topic.minValue) / (topic.maxValue - topic.minValue);

        String barCssClass = topic.style;
        if (i == TopicId::CO2) barCssClass += getIAQLevel(topicValue);
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
    Html.writeSectionEnd();
}


void writeHourStats()
{
    Html.writeSectionStart(F("Last 24 hours"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    Html.writeHeaderCell(F("T (°C)"));
    Html.writeHeaderCell(F("Fan (%)"));
    Html.writeHeaderCell(F("CO<sub>2</sub> (ppm)"));
    Html.writeRowEnd();

    float minCO2, maxCO2;
    getCO2Range(minCO2, maxCO2);

    HttpResponse.printf(F("<p>Min CO<sub>2</sub>: %0.0f, Max CO<sub>2</sub>: %0.0f</p>"), minCO2, maxCO2);

    TopicLogEntry* statsEntryPtr = HourStats.getFirstEntry();
    while (statsEntryPtr != nullptr)
    {
        float t = statsEntryPtr->getAverage(TopicId::Temperature);
        float fan = statsEntryPtr->getAverage(TopicId::Fan) * 100;
        float co2 = statsEntryPtr->getAverage(TopicId::CO2);

        float barValue = (co2 - minCO2) / (maxCO2 - minCO2);

        String barClass = F("iaq");
        barClass += getIAQLevel(co2);
        barClass += F("Bar");

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", statsEntryPtr->time));
        Html.writeCell(t);
        Html.writeCell(fan, F("%0.0f"));
        Html.writeCell(co2, F("%0.0f"));
        Html.writeCellStart(F("graph"));
        Html.writeBar(barValue, barClass, false);
        Html.writeCellEnd();
        Html.writeRowEnd();

        statsEntryPtr = HourStats.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void getCO2Range(float& min, float& max)
{
    min = 6666;
    max = 0;

    TopicLogEntry* statsEntryPtr = HourStats.getFirstEntry();
    while (statsEntryPtr != nullptr)
    {
        float co2 = statsEntryPtr->getAverage(TopicId::CO2);
        min = std::min(min, co2);
        max = std::max(max, co2);
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
        float topicValue = currentTopicValues[i];

        if (i > 0) HttpResponse.print(F(", "));
        HttpResponse.printf(
            F(" \"%s\": %s"),
            topic.label,
            topic.formatValue(topicValue, false));
    }

    HttpResponse.println(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void handleHttpIAQLogRequest()
{
    Tracer tracer(F(__func__));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((IAQLog.count() - 1) / IAQ_LOG_PAGE_SIZE) + 1;

    Html.writeHeader(F("IAQ log"), Nav);
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
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        for (int k = 0; k < NUMBER_OF_MONITORED_TOPICS; k++)
        {
            Html.writeCell(MonitoredTopics[k].formatValue(logEntryPtr->getAverage(k), false, 1));
        }
        Html.writeRowEnd();

        logEntryPtr = IAQLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("FTP Sync"), Nav);

    HttpResponse.println(F("<pre>"));
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println(F("</pre>"));

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed!"));
 
    Html.writeHeading(F("CSV header"), 2);
    HttpResponse.print("<pre>Time");
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
    {
        HttpResponse.print(";");
        HttpResponse.print(MonitoredTopics[i].label);
    }
    HttpResponse.println(F("</pre>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), Nav);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv(event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"), F("grid"));
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1, F("password"));
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1, F("password"));
    Html.writeNumberBox(CFG_FTP_ENTRIES, F("FTP sync entries"), PersistentData.ftpSyncEntries, 0, IAQ_LOG_SIZE);
    Html.writeNumberBox(CFG_FAN_THRESHOLD, F("Fan threshold"), PersistentData.fanCO2Threshold, 400, 5000);
    Html.writeNumberBox(CFG_FAN_HYSTERESIS, F("Fan hysteresis"), PersistentData.fanCO2Hysteresis, 0, 1000);
    Html.writeTextBox(CFG_FAN_OFF_FROM, F("Fan off from"), formatTime("%H:%M", getStartOfDay(currentTime) + PersistentData.fanOffFromMinutes * 60), 5);
    Html.writeTextBox(CFG_FAN_OFF_TO, F("Fan off to"), formatTime("%H:%M", getStartOfDay(currentTime) + PersistentData.fanOffToMinutes * 60), 5);
    Html.writeNumberBox(CFG_TEMP_OFFSET, F("Temp offset"), PersistentData.tOffset, -5, 5, 1);
    Html.writeSubmitButton(F("Save"));
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
        WiFiSM.reset();
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


uint16_t parseMinutes(const String& input)
{
    int hours = 0;
    int minutes = 0;
    if (sscanf(input.c_str(), "%d:%d", &hours, &minutes) != 2)
        WiFiSM.logEvent(F("Can't parse '%s' as hours/minutes"), input.c_str());

    return hours * 60 + minutes;
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
    PersistentData.fanCO2Threshold = WebServer.arg(CFG_FAN_THRESHOLD).toInt();
    PersistentData.fanCO2Hysteresis = WebServer.arg(CFG_FAN_HYSTERESIS).toInt();
    PersistentData.tOffset = WebServer.arg(CFG_TEMP_OFFSET).toFloat();

    PersistentData.fanOffFromMinutes = parseMinutes(WebServer.arg(CFG_FAN_OFF_FROM));
    PersistentData.fanOffToMinutes = parseMinutes(WebServer.arg(CFG_FAN_OFF_TO));

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    IAQSensor.setTemperatureOffset(PersistentData.tOffset);

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
