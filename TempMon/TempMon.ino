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
#include <Navigation.h>
#include <Log.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "TempLogEntry.h"
#include "TempStatsEntry.h"
#include "DayStatistics.h"
#include "GoodWeUDP.h"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define GOODWE_POLL_INTERVAL (5 * 60)
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (60 * 60)
#define HOUR_LOG_INTERVAL (30 * 60)
#define TEMP_POLL_INTERVAL 6
#define TEMP_LOG_AGGREGATIONS 10
#define TEMP_LOG_SIZE 250
#define NIGHT_OFFSET_DELAY (10 * 60)

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

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";
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

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TempLogEntry> TempLog(TEMP_LOG_SIZE);
StaticLog<TempStatsEntry> HourStats(24 * 2); // 24 hrs
DayStatistics DayStats;
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Navigation Nav;

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
U8G2_SSD1327_MIDAS_128X128_F_HW_I2C Display(U8G2_R1, /* reset=*/ U8X8_PIN_NONE);
GoodWeUDP GoodWe;

time_t currentTime = 0;
time_t nightTime = 0;
time_t dayTime = 0;
time_t pollTempTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;
time_t initGoodWeTime = 0;
time_t lastGoodWeInitTime = 0;

int ftpSyncEntries = 0;
TempLogEntry newTempLogEntry;

TempLogEntry* lastTempLogEntryPtr = nullptr;
TempStatsEntry* lastHoutStatsEntryPtr = nullptr;

bool newSensorFound = false;
bool hasOutsideSensor = false;
bool measuringTemp = false;

float tInside = 0;
float tOutside = 0;

char stringBuffer[16];
int displayPage = 0;


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


void logSensorInfo(const char* name, DeviceAddress addr, float offset)
{
    char message[64];
    if (TempSensors.isConnected(addr))
    {
        snprintf(
            message,
            sizeof(message),
            "%s sensor address: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. Offset: %0.2f",
            name,
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
            offset
            );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "%s sensor is not connected.",
            name
            );
    }

    String event(message);
    logEvent(event);
}


void initTempSensors()
{
    Tracer tracer(F("initTempSensors"));

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (TempSensors.getDS18Count() > 0 && !TempSensors.validFamily(PersistentData.tInsideSensorAddress))
    {
        newSensorFound = TempSensors.getAddress(PersistentData.tInsideSensorAddress, 0);
        if (!newSensorFound)
        {
            logEvent(F("ERROR: Unable to obtain inside sensor address."));
        }
    }
    if (TempSensors.getDS18Count() > 1 && !TempSensors.validFamily(PersistentData.tOutsideSensorAddress))
    {
        newSensorFound = TempSensors.getAddress(PersistentData.tOutsideSensorAddress, 1);
        if (!newSensorFound)
        {
            logEvent(F("ERROR: Unable to obtain outside sensor address."));
        }
    }

    if (newSensorFound)
    {
        PersistentData.writeToEEPROM();
    }
}

// Boot code
void setup() 
{
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    Serial.begin(115200);
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
            .label = PSTR("Temperature log"),
            .urlPath = PSTR("templog"),
            .handler = handleHttpTempLogRequest
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
            .icon = Files[CalibrateIcon],
            .label = PSTR("Calibrate"),
            .urlPath = PSTR("calibrate"),
            .handler= handleHttpCalibrateFormRequest,
            .postHandler = handleHttpCalibrateFormPost
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
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (!GoodWe.begin())
        WiFiSM.logEvent(F("GoodWe init failed: %s"), GoodWe.getLastError());

    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    initTempSensors();
    hasOutsideSensor = TempSensors.isConnected(PersistentData.tOutsideSensorAddress);

    logSensorInfo("Inside", PersistentData.tInsideSensorAddress, PersistentData.tInsideOffset);
    logSensorInfo("Outside", PersistentData.tOutsideSensorAddress, PersistentData.tOutsideOffset);    

    Display.setBusClock(400000);
    Display.begin();
    displayMessage("Booting...");

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
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

void onWiFiTimeSynced()
{
    pollTempTime = currentTime;
    newTempLogEntry.time = currentTime;
    if (!WiFiSM.isInAccessPointMode())
        initGoodWeTime = currentTime;
}


void onWiFiInitialized()
{
    if (currentTime % SECONDS_PER_DAY == 0)
    {
        DayStats.reset();
    }

    if (currentTime >= pollTempTime)
    {
        pollTempTime += TEMP_POLL_INTERVAL;
        if (TempSensors.getDS18Count() > 0)
        {
            TempSensors.requestTemperatures();
            measuringTemp = true;
        }
    }

    if (measuringTemp && TempSensors.isConversionComplete())
    {
        measuringTemp = false;
        digitalWrite(LED_BUILTIN, LED_ON);

        float tInsideMeasured = TempSensors.getTempC(PersistentData.tInsideSensorAddress);
        if (tInsideMeasured != DEVICE_DISCONNECTED_C)
        {
            // At "night time" (when WiFi is sleeping), the inside temperature sensor may sense less heat
            // and thus may require a different offset.
            if ((nightTime != 0) && (currentTime >= nightTime))
                tInside = tInsideMeasured + PersistentData.tInsideNightOffset;
            else
                tInside = tInsideMeasured + PersistentData.tInsideOffset;
        }

        float tOutsideMeasured = TempSensors.getTempC(PersistentData.tOutsideSensorAddress);
        if (tOutsideMeasured != DEVICE_DISCONNECTED_C)
            tOutside = tOutsideMeasured + PersistentData.tOutsideOffset;

        digitalWrite(LED_BUILTIN, LED_OFF);

        updateTempLog();
        DayStats.update(tInside, tOutside, currentTime);
        displayData();
    }

    if (!WiFiSM.isConnected())
    {
        // WiFi connection is lost. 
        // This starts "night time" after a while (to let the sensor cool down a bit). 
        if (nightTime == 0) nightTime = currentTime  + NIGHT_OFFSET_DELAY;

        // Try to find & initialize GoodWe(s) as soon as WiFi connection is restored
        initGoodWeTime = currentTime;

        // Skip stuff that requires a WiFi connection.
        return;
    }

    if (nightTime != 0)
    {
        // WiFi is connected. 
        // This stops "night time" after a while (to let the sensor warm up a bit). 
        if (dayTime == 0) dayTime = currentTime + NIGHT_OFFSET_DELAY;
        else if (currentTime >= dayTime)
        {
            nightTime = 0;
            dayTime = 0;
        }
    }

    if ((initGoodWeTime != 0) && (currentTime >= initGoodWeTime))
    {
        if (initializeGoodWe())
        {
            lastGoodWeInitTime = currentTime;
            initGoodWeTime = 0;
        }
        else
        {
            TRACE(F("Unable to initialize GoodWe; retry in %d s.\n"), GOODWE_POLL_INTERVAL);
            initGoodWeTime += GOODWE_POLL_INTERVAL;
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


bool initializeGoodWe()
{
    Tracer tracer(F("initializeGoodWe"));

    int devicesFound = GoodWe.discover();
    if (devicesFound < 0)
        WiFiSM.logEvent("GoodWe discovery failed: %s", GoodWe.getLastError());
    if (devicesFound <= 0)
        return false;

    bool result = true;
    for (int i = 0; i < devicesFound; i++)
    {
        GoodWeInstance* goodWeInstancePtr = GoodWe.getInstance(i);
        WiFiSM.logEvent(F("Found GoodWe: %s"), goodWeInstancePtr->getIPAddress().toString().c_str());

        String apInfo;
        if (goodWeInstancePtr->sendATCommand(F("WSLK"), apInfo))
        {
            WiFiSM.logEvent(F("Connected to: %s"), apInfo.c_str());

            bool reset = false;
            if (!ensureGoodWeProperty(goodWeInstancePtr, F("WMODE"), F("STA"), reset)
                || !ensureGoodWeProperty(goodWeInstancePtr, F("WSDNS"), WiFi.dnsIP().toString(), reset))
                result = false;

            if (reset)
            {
                if (goodWeInstancePtr->sendATCommand(F("Z")))
                    WiFiSM.logEvent(F("WiFi module resetting"));
                else
                {
                    WiFiSM.logEvent(GoodWe.getLastError());
                    result = false;
                }
            }
        }
        else
        {
            WiFiSM.logEvent(GoodWe.getLastError());
            result = false;
        }

        delete goodWeInstancePtr;
    }

    return result;
}


bool ensureGoodWeProperty(GoodWeInstance* goodWeInstancePtr, const String& name, const String& value, bool& changed)
{
    String currentValue;
    if (!goodWeInstancePtr->sendATCommand(name, currentValue))
    {
        WiFiSM.logEvent(GoodWe.getLastError());
        return false;
    }

    WiFiSM.logEvent(F("%s = %s"), name.c_str(), currentValue.c_str());
    if (currentValue == value)
        return true;

    String setCommand = name;
    setCommand += "=";
    setCommand += value;
    if (!goodWeInstancePtr->sendATCommand(setCommand))
    {
        WiFiSM.logEvent(GoodWe.getLastError());
        return false;
    }

    WiFiSM.logEvent(F("Changed %s to %s"), name.c_str(), value.c_str());
    changed = true;
    return true;
}


float getBarValue(float t, float tMin = 10, float tMax = 30)
{
    float result = std::max(t - tMin, 0.0F) / (tMax - tMin);
    TRACE(F("getBarValue(%0.1f, %0.1f, %0.1f) = %0.2f\n"), t, tMin, tMax, result);
    return result;
}


void displayMessage(const char* msg)
{
    Tracer tracer(F("displayMessage"), msg);

    Display.clearBuffer();
    Display.setFont(u8g2_font_10x20_tf);
    Display.drawStr(0, 20, msg);
    Display.sendBuffer();
}


void displayData()
{
    Tracer tracer(F("displayData"));

    bool nightMode = WiFiSM.getState() < WiFiInitState::Connected;
    Display.setPowerSave(nightMode ? 1 : 0);
    if (nightMode) return;

    Display.clearBuffer();

    Display.setFont(u8g2_font_logisoso58_tr);
    snprintf(stringBuffer, sizeof(stringBuffer), "%0.1f", tInside);
    Display.drawStr(0, 60, stringBuffer);

    if (displayPage++ == 0)
        drawDayStats();
    else
    {
        drawTempGraph();
        displayPage = 0;
    }

    Display.sendBuffer();
}


void drawDayStats()
{
    char strBuf[16];

    Display.setFont(u8g2_font_5x7_tr);
    Display.drawStr(0, 79, "Min");
    Display.drawStr(0, 111, "Max");

    Display.setFont(u8g2_font_10x20_tr);
    snprintf(
        stringBuffer,
        sizeof(stringBuffer),
        "%0.1f @ %s",
        DayStats.tInsideMin,
        formatTime("%H:%M", DayStats.insideMinTime) 
        );
    Display.drawStr(0, 95, stringBuffer);
    snprintf(
        stringBuffer,
        sizeof(stringBuffer),
        "%0.1f @ %s",
        DayStats.tInsideMax,
        formatTime("%H:%M", DayStats.insideMaxTime) 
        );
    Display.drawStr(0, 127, stringBuffer);
}


void drawDottedLine(uint8_t startX, uint8_t y, uint8_t length, uint8_t interval)
{
    for (int x = startX; x < (startX + length); x += interval)
        Display.drawPixel(x, y);
}


void drawTempGraph()
{
    const uint8_t graphX = 29;
    const uint8_t graphWidth = 48 * 2;
    const uint8_t graphHeight = 64;
    uint8_t graphY = Display.getDisplayHeight() - 1;

    float tMin, tMax;
    getTemperatureRange(tMin, tMax);

    // Draw Y axis labels
    Display.setFont(u8g2_font_5x7_tr);
    snprintf(stringBuffer, sizeof(stringBuffer), "%0.1f", tMin);
    Display.drawStr(0, graphY, stringBuffer);
    snprintf(stringBuffer, sizeof(stringBuffer), "%0.1f", tMax);
    Display.drawStr(0, graphY - graphHeight + 8, stringBuffer);

    drawDottedLine(graphX, graphY, graphWidth, 4);
    drawDottedLine(graphX, graphY - graphHeight, graphWidth, 4);

    uint8_t x = graphX;
    uint8_t lastY = 0;
    TempStatsEntry* hourStatsEntryPtr = HourStats.getFirstEntry();
    while (hourStatsEntryPtr != nullptr)
    {
        uint8_t y = graphY - getBarValue(hourStatsEntryPtr->getAvgTInside(), tMin, tMax) * graphHeight;

        if (lastY != 0)
            Display.drawLine(x-2, lastY, x, y);

        x += 2;
        lastY = y;
        hourStatsEntryPtr = HourStats.getNextEntry();
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
        FTPClient.end();
        return false;
    }

    bool success = true;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (ftpSyncEntries > 0)
        {
            writeTempLogCsv(TempLog.getEntryFromEnd(ftpSyncEntries), dataClient);
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


void updateTempLog()
{
    newTempLogEntry.update(tInside, tOutside);
    if (newTempLogEntry.count == TEMP_LOG_AGGREGATIONS)
    {
        if ((lastTempLogEntryPtr == nullptr) || !newTempLogEntry.equals(lastTempLogEntryPtr))
        {
            lastTempLogEntryPtr = TempLog.add(&newTempLogEntry);
            newTempLogEntry.time = currentTime;
            if (PersistentData.isFTPEnabled())
            {
                if (++ftpSyncEntries > TEMP_LOG_SIZE)
                    ftpSyncEntries = TEMP_LOG_SIZE;
                if (ftpSyncEntries >= PersistentData.ftpSyncEntries)
                    syncFTPTime = currentTime;
            }
        }
        newTempLogEntry.reset(); // Moving average
    }

    if ((lastHoutStatsEntryPtr == nullptr) || (currentTime >= lastHoutStatsEntryPtr->time + HOUR_LOG_INTERVAL))
    {
        TempStatsEntry newHourStatsEntry;
        newHourStatsEntry.time = currentTime - (currentTime % HOUR_LOG_INTERVAL);
        lastHoutStatsEntryPtr = HourStats.add(&newHourStatsEntry);
    }
    lastHoutStatsEntryPtr->update(tInside, tOutside);
}


void test(String message)
{
    Tracer tracer(F("test"), message.c_str());

    if (message.startsWith("testL"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            logEvent(F("Test event to fill the event log"));
            yield();
        }
    }
    else if (message.startsWith("testT"))
    {
        float tInside = 15;
        for (int i = 0; i < 48; i++)
        {
            TempStatsEntry tempLogEntry;
            tempLogEntry.time = currentTime + (i * 1800);
            tempLogEntry.update(tInside, tInside - 10);
            lastHoutStatsEntryPtr = HourStats.add(&tempLogEntry);
            if (tInside++ == 40) tInside = 15;
        }
    }
    else if (message.startsWith("testR"))
    {
        WiFiSM.reset();
    }
}


void getTemperatureRange(float& tMin, float& tMax)
{
    tMin = 100;
    tMax = -100;
    TempStatsEntry* logEntryPtr = HourStats.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        float avgTInside = logEntryPtr->getAvgTInside();
        tMin = std::min(tMin, avgTInside);
        tMax = std::max(tMax, avgTInside);
        if (hasOutsideSensor)
        {
            float avgTOutside = logEntryPtr->getAvgTOutside();
            tMin = std::min(tMin, avgTOutside);
            tMax = std::max(tMax, avgTOutside);
        }
        logEntryPtr = HourStats.getNextEntry();
    }

    tMax = std::max(tMax, tMin + 0.1F); // Prevent division by zero
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    if (newSensorFound)
    {
        handleHttpCalibrateFormRequest();
        return;
    }

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
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
    Html.writeRow(F("GoodWe init"), formatTime("%a %H:%M", lastGoodWeInitTime));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeTemperatures();
    writeHourStats();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeTemperatures()
{
    Html.writeSectionStart(F("Temperatures"));
    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Sensor"));
    Html.writeHeaderCell(F("Current"));
    Html.writeHeaderCell(F("Min"));
    Html.writeHeaderCell(F("Max"));
    Html.writeRowEnd();

    Html.writeRowStart();
    Html.writeCell(F("Inside"));
    Html.writeCell(F("%0.1f °C"), tInside);
    Html.writeCell(F("<div>%0.1f °C</div><div>@ %s</div>"), DayStats.tInsideMin, formatTime("%H:%M", DayStats.insideMinTime));
    Html.writeCell(F("<div>%0.1f °C</div><div>@ %s</div>"), DayStats.tInsideMax, formatTime("%H:%M", DayStats.insideMaxTime));
    Html.writeRowEnd();

    if (hasOutsideSensor)
    {
        Html.writeRowStart();
        Html.writeCell(F("Outside"));
        Html.writeCell(F("%0.1f °C"), tOutside);
        Html.writeCell(F("<div>%0.1f °C</div><div>@ %s</div>"), DayStats.tOutsideMin, formatTime("%H:%M", DayStats.outsideMinTime));
        Html.writeCell(F("<div>%0.1f °C</div><div>@ %s</div>"), DayStats.tOutsideMax, formatTime("%H:%M", DayStats.outsideMaxTime));
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}

void writeHourStats()
{
    float tMin, tMax;
    getTemperatureRange(tMin, tMax);

    Html.writeSectionStart(F("Last 24 hours"));
    Html.writeParagraph(F("Min: %0.1f°C, Max: %0.1f°C"), tMin, tMax);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("T<sub>inside</sub> (°C)"), 3);
    if (hasOutsideSensor)
        Html.writeHeaderCell(F("T<sub>outside</sub> (°C)"), 3);
    Html.writeRowEnd();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Min"));
    Html.writeHeaderCell(F("Max"));
    Html.writeHeaderCell(F("Avg"));
    if (hasOutsideSensor)
    {
        Html.writeHeaderCell(F("Min"));
        Html.writeHeaderCell(F("Max"));
        Html.writeHeaderCell(F("Avg"));
    }
    Html.writeRowEnd();

    TempStatsEntry* logEntryPtr = HourStats.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        Html.writeCell(logEntryPtr->minTInside);
        Html.writeCell(logEntryPtr->maxTInside);
        Html.writeCell(logEntryPtr->getAvgTInside());
        if (hasOutsideSensor)
        {
            Html.writeCell(logEntryPtr->minTOutside);
            Html.writeCell(logEntryPtr->maxTOutside);
            Html.writeCell(logEntryPtr->getAvgTOutside());
        }

        float outsideBar = hasOutsideSensor ? getBarValue(logEntryPtr->getAvgTOutside(), tMin, tMax) : 0;
        float insideBar = getBarValue(logEntryPtr->getAvgTInside(), tMin, tMax);  

        Html.writeCellStart(F("graph"));
        Html.writeStackedBar(
            outsideBar,
            insideBar - outsideBar,
            F("tOutsideBar"),
            F("tInsideBar"),
            false,
            false
            );
        Html.writeCellEnd();

        Html.writeRowEnd();

        logEntryPtr = HourStats.getNextEntry();
    }
    HttpResponse.println(F("</table>"));
}


void writeJsonFloat(String name, float value)
{
    HttpResponse.printf(F("\"%s\": %0.1f"), name.c_str(), value);
}

void handleHttpJsonRequest()
{
    Tracer tracer(F("handleHttpJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));
    writeJsonFloat(F("Tin"), tInside);
    HttpResponse.print(F(", "));
    writeJsonFloat(F("Tout"), tOutside);
    HttpResponse.print(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void writeTempLogCsv(TempLogEntry* tempLogEntryPtr, Print& destination)
{
    while (tempLogEntryPtr != nullptr)
    {
        destination.printf(
            "%s;%0.1f;%0.1f\r\n",
            formatTime("%F %H:%M", tempLogEntryPtr->time),
            tempLogEntryPtr->getAvgTinside(),
            tempLogEntryPtr->getAvgToutside()
            );

        tempLogEntryPtr = TempLog.getNextEntry();
    }
}


void handleHttpTempLogRequest()
{
    Tracer tracer(F("handleHttpTempLogRequest"));

    HttpResponse.clear();
    HttpResponse.println(F("Time;Tinside;Toutside"));

    writeTempLogCsv(TempLog.getFirstEntry(), HttpResponse);

    WebServer.send(200, ContentTypeText, HttpResponse);
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
        Html.writeParagraph(F("Failed!"));
 
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));

    Html.writeHeader(F("Calibrate"), Nav);

    if (TempSensors.getDS18Count() < 1)
    {
        Html.writeHeading(F("Missing temperature sensors"), 2);
        Html.writeParagraph(F("Number of temperature sensors detected: %d"), TempSensors.getDS18Count());
    }
    else
    {
        float tInsideMeasured = TempSensors.getTempC(PersistentData.tInsideSensorAddress);
    
        Html.writeFormStart(F("/calibrate"));
        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell(F("Sensor"));
        Html.writeHeaderCell(F("Measured"));
        Html.writeHeaderCell(F("Offset"));
        Html.writeHeaderCell(F("Effective"));
        Html.writeRowEnd();
   
        HttpResponse.printf(
            F("<tr><td>Inside</td><td>%0.2f °C<td><input type=\"text\" name=\"tInsideOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
            tInsideMeasured,
            PersistentData.tInsideOffset,
            tInsideMeasured + PersistentData.tInsideOffset
            );
    
        HttpResponse.printf(
            F("<tr><td>Inside (night)</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tInsideNightOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
            tInsideMeasured,
            PersistentData.tInsideNightOffset,
            tInsideMeasured + PersistentData.tInsideNightOffset
            );

        if (hasOutsideSensor)
        {
            float tOutsideMeasured = TempSensors.getTempC(PersistentData.tOutsideSensorAddress);
            HttpResponse.printf(
                F("<tr><td>Outside</td><td>%0.2f °C<td><input type=\"text\" name=\"tOutsideOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
                tOutsideMeasured,
                PersistentData.tOutsideOffset,
                tOutsideMeasured + PersistentData.tOutsideOffset        
                );
        }
    
        Html.writeTableEnd();

        if (hasOutsideSensor)
        {
            Html.writeCheckbox(F("swapInOut"), F("Swap input and output sensors"), false);
            HttpResponse.println(F("<br/>"));
        }

        Html.writeSubmitButton(F("Calibrate"));
        Html.writeFormEnd();
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    PersistentData.tInsideOffset = WebServer.arg("tInsideOffset").toFloat();
    PersistentData.tInsideNightOffset = WebServer.arg("tInsideNightOffset").toFloat();

    if (WebServer.hasArg("tOutsideOffset"))
    {
        PersistentData.tOutsideOffset = WebServer.arg("tOutsideOffset").toFloat();
    }

    if (WebServer.hasArg("swapInOut"))
    {
        DeviceAddress tInsideSensorAddress;
        memcpy(tInsideSensorAddress, PersistentData.tInsideSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tInsideSensorAddress, PersistentData.tOutsideSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tOutsideSensorAddress, tInsideSensorAddress, sizeof(DeviceAddress));
    }

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    newSensorFound = false;

    handleHttpCalibrateFormRequest();
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), Nav);

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
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1, F("password"));
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1, F("password"));
    Html.writeTextBox(CFG_FTP_ENTRIES, F("FTP sync entries"), String(PersistentData.ftpSyncEntries), 3);
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

    PersistentData.ftpSyncEntries = WebServer.arg(CFG_FTP_ENTRIES).toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
