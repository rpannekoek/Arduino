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
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "TempLogEntry.h"
#include "DayStatistics.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (60 * 60)
#define TEMP_LOG_INTERVAL (30 * 60)
#define TEMP_POLL_INTERVAL 10

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<TempLogEntry> TempLog(24 * 2); // 24 hrs
DayStatistics DayStats;
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
U8G2_SSD1327_MIDAS_128X128_F_HW_I2C Display(U8G2_R1, /* reset=*/ U8X8_PIN_NONE);

time_t currentTime = 0;
time_t pollTempTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

TempLogEntry* lastTempLogEntryPtr = nullptr;

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

    Serial.begin(74880); // Use same baudrate as bootloader
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
    WebServer.on("/sync", handleHttpFtpSyncRequest);
    WebServer.on("/calibrate", HTTP_GET, handleHttpCalibrateFormRequest);
    WebServer.on("/calibrate", HTTP_POST, handleHttpCalibrateFormPost);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

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

    delay(10);
}

void onWiFiTimeSynced()
{
    pollTempTime = currentTime;
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
            tInside = tInsideMeasured + PersistentData.tInsideOffset;

        float tOutsideMeasured = TempSensors.getTempC(PersistentData.tOutsideSensorAddress);
        if (tOutsideMeasured != DEVICE_DISCONNECTED_C)
            tOutside = tOutsideMeasured + PersistentData.tOutsideOffset;

        digitalWrite(LED_BUILTIN, LED_OFF);

        updateTempLog();
        DayStats.update(tInside, tOutside, currentTime);
        displayData();
    }

    if (WiFiSM.getState() == WiFiInitState::ConnectionLost)
        return;

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

    bool nightMode = WiFiSM.getState() == WiFiInitState::ConnectionLost;
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
    TempLogEntry* logEntryPtr = TempLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        uint8_t y = graphY - getBarValue(logEntryPtr->getAvgTInside(), tMin, tMax) * graphHeight;

        if (lastY != 0)
            Display.drawLine(x-2, lastY, x, y);

        x += 2;
        lastY = y;
        logEntryPtr = TempLog.getNextEntry();
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
        /* TODO
        if (EnergyLog.count() > 1)
        {
            EnergyLogEntry* energyLogEntryPtr = EnergyLog.getEntryFromEnd(2);
            if (energyLogEntryPtr != nullptr)
            {
                dataClient.printf(
                    "\"%s\";%0.1f;%0.1f\r\n",
                    formatTime("%F", energyLogEntryPtr->time),
                    energyLogEntryPtr->energyIn,
                    energyLogEntryPtr->energyOut
                    );
            }
        }
        else*/ if (printTo != nullptr)
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
    if ((lastTempLogEntryPtr == nullptr) || (currentTime >= lastTempLogEntryPtr->time + TEMP_LOG_INTERVAL))
    {
        TempLogEntry newTempLogEntry;
        newTempLogEntry.time = currentTime - (currentTime % TEMP_LOG_INTERVAL);
        lastTempLogEntryPtr = TempLog.add(&newTempLogEntry);
    }
    lastTempLogEntryPtr->update(tInside, tOutside);
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
            TempLogEntry tempLogEntry;
            tempLogEntry.time = currentTime + (i * 1800);
            tempLogEntry.update(tInside, tInside - 10);
            lastTempLogEntryPtr = TempLog.add(&tempLogEntry);
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
    TempLogEntry* logEntryPtr = TempLog.getFirstEntry();
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
        logEntryPtr = TempLog.getNextEntry();
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

    const char* ftpSync;
    if (PersistentData.ftpServer[0] == 0)
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>Temperature Monitor status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSync);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    if (WiFiSM.shouldPerformAction("reset"))
    {
        Display.begin();
        displayMessage("Reset");
        HttpResponse.println(F("<p>Display reset.</p>"));
    }
    else
        HttpResponse.printf(F("<p><a href=\"?reset=%u\">Reset display</a></p>\r\n"), currentTime);

    HttpResponse.println(F("<h1>Current values</h1>"));
    writeCurrentValues();

    HttpResponse.println(F("<h1>Today</h1>"));
    writeDayStats();

    HttpResponse.println(F("<h1>Last 24 hours</h1>"));
    writeTempLog();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeCurrentValues()
{
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>inside</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        tInside
        );
    Html.writeBar(getBarValue(tInside), F("tInsideBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    if (hasOutsideSensor)
    {
        HttpResponse.printf(
            F("<tr><th>T<sub>inside</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
            tOutside
            );
        Html.writeBar(getBarValue(tOutside), F("tOutsideBar"), true, false);
        HttpResponse.println(F("</td></tr>"));
    }
    HttpResponse.println(F("</table>"));
}


void writeDayStats()
{
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th/><th>Min</th><th>Max</th></tr>"));
    HttpResponse.printf(
        F("<tr><th>Inside</th><td>%0.1f°C @ %s</td>"),
        DayStats.tInsideMin,
        formatTime("%H:%M", DayStats.insideMinTime)
        );
    HttpResponse.printf(
        F("<td>%0.1f°C @ %s</td></tr>\r\n"),
        DayStats.tInsideMax,
        formatTime("%H:%M", DayStats.insideMaxTime)
        );

    if (hasOutsideSensor)
    {
        HttpResponse.printf(
            F("<tr><th>Outside</th><td>%0.1f°C @ %s</td>"),
            DayStats.tOutsideMin,
            formatTime("%H:%M", DayStats.outsideMinTime)
            );
        HttpResponse.printf(
            F("<td>%0.1f°C @ %s</td></tr>\r\n"),
            DayStats.tOutsideMax,
            formatTime("%H:%M", DayStats.outsideMaxTime)
            );
    }
    HttpResponse.println(F("</table>"));
}


void writeTempLog()
{
    float tMin, tMax;
    getTemperatureRange(tMin, tMax);

    HttpResponse.printf(F("<p>Min: %0.1f°C, Max: %0.1f°C</p>\r\n"), tMin, tMax);
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th colspan='3'>T<sub>inside</sub> (°C)</th><th colspan='3'>T<sub>outside</sub> (°C)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));

    TempLogEntry* logEntryPtr = TempLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            logEntryPtr->minTInside,
            logEntryPtr->maxTInside,
            logEntryPtr->getAvgTInside(),
            logEntryPtr->minTOutside,
            logEntryPtr->maxTOutside,
            logEntryPtr->getAvgTOutside()
            );

        float outsideBar = hasOutsideSensor ? getBarValue(logEntryPtr->getAvgTOutside(), tMin, tMax) : 0;
        float insideBar = getBarValue(logEntryPtr->getAvgTInside(), tMin, tMax);  

        Html.writeStackedBar(
            outsideBar,
            insideBar - outsideBar,
            F("tOutsideBar"),
            F("tInsideBar"),
            false,
            false
            );

        HttpResponse.println(F("</td></tr>"));
        logEntryPtr = TempLog.getNextEntry();
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


void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));

    Html.writeHeader(F("Calibrate sensors"), true, true);

    if (TempSensors.getDS18Count() < 1)
    {
        HttpResponse.println(F("<h2>Missing temperature sensors</h2>"));
        HttpResponse.printf(F("<p>Number of temperature sensors detected: %d</p>\r\n"), TempSensors.getDS18Count());
    }
    else
    {
        float tInsideMeasured = TempSensors.getTempC(PersistentData.tInsideSensorAddress);
    
        HttpResponse.println(F("<form action=\"/calibrate\" method=\"POST\">"));
        HttpResponse.println(F("<table>"));
        HttpResponse.println(F("<tr><th>Sensor</th><th>Measured</th><th>Offset</th><th>Effective</th></tr>"));
    
        HttpResponse.printf(
            F("<tr><td>T<sub>in</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tInsideOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
            tInsideMeasured,
            PersistentData.tInsideOffset,
            tInsideMeasured + PersistentData.tInsideOffset
            );
    
        if (hasOutsideSensor)
        {
            float tOutsideMeasured = TempSensors.getTempC(PersistentData.tOutsideSensorAddress);
            HttpResponse.printf(
                F("<tr><td>T<sub>buffer</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tBufferOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
                tOutsideMeasured,
                PersistentData.tOutsideOffset,
                tOutsideMeasured + PersistentData.tOutsideOffset        
                );
        }
    
        HttpResponse.println(F("</table>"));

        if (hasOutsideSensor)
        {
            Html.writeCheckbox(F("swapInOut"), F("Swap input and output sensors"), false);
            HttpResponse.println(F("<br/>"));
        }

        HttpResponse.println(F("<input type=\"submit\">"));
        HttpResponse.println(F("</form>"));
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    PersistentData.tInsideOffset = WebServer.arg("tInsideOffset").toFloat();
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

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
