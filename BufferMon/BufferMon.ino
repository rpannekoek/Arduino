#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "BufferLogEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (60 * 60)
#define BUFFER_LOG_INTERVAL (30 * 60)

#define MAX_TEMP_VALVE_PIN D2
#define MAX_TEMP_DELTA_T 5
#define LED_EXTERNAL_PIN D6

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_TZ_OFFSET F("tzOffset")
#define CFG_MAX_TEMP F("maxTemp")

const char *ContentTypeHtml = "text/html;charset=UTF-8";

ESPWebServer WebServer(80);          // Default HTTP port
WiFiNTP TimeServer(SECONDS_PER_DAY); // Synchronize daily
WiFiFTPClient FTPClient(2000);       // 2 sec timeout
StringBuilder HttpResponse(16384);   // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 70);
Log<const char> EventLog(EVENT_LOG_LENGTH);
Log<BufferLogEntry> BufferLog(24 * 2); // 24 hrs
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);

time_t currentTime = 0;
time_t bufferLogTime = 0;

BufferLogEntry *lastBufferLogEntryPtr = nullptr;

float tBuffer = 0;
bool maxTempValveActivated = false;
bool maxTempValveOverride = false;
bool calibrationRequired = false;


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
            offset);
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "ERROR: %s sensor is not connected.",
            name);
    }

    String event(message);
    logEvent(event);
}

void setMaxTempValve(bool activated)
{
    maxTempValveActivated = activated;
    digitalWrite(MAX_TEMP_VALVE_PIN, maxTempValveActivated ? 1 : 0);

    String message = F("Max temperature valve set ");
    if (activated)
        message += F("on");
    else
        message += F("off");
    
    logEvent(message);
}

// Boot code
void setup()
{
    // Turn built-in and external LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);
    pinMode(LED_EXTERNAL_PIN, OUTPUT);
    digitalWrite(LED_EXTERNAL_PIN, LED_ON);

    Serial.begin(74880); // Use same baudrate as bootloader
    Serial.setTimeout(1000);
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

#ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
#endif

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    Html.setTitlePrefix(PersistentData.hostName);

    SPIFFS.begin();

    const char *cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/calibrate", HTTP_GET, handleHttpCalibrateFormRequest);
    WebServer.on("/calibrate", HTTP_POST, handleHttpCalibrateFormPost);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (!TempSensors.validFamily(PersistentData.tempSensorAddress))
    {
        // Device address not initialized; obtain & store in persistent data.
        calibrationRequired = true;
        if (!TempSensors.getAddress(PersistentData.tempSensorAddress, 0))
        {
            logEvent(F("ERROR: Unable to obtain temperature sensor address."));
            calibrationRequired = false;
        }
        PersistentData.writeToEEPROM();
    }

    logSensorInfo("Buffer", PersistentData.tempSensorAddress, PersistentData.tempOffset);

    TempSensors.requestTemperatures();

    Tracer::traceFreeHeap();

    pinMode(MAX_TEMP_VALVE_PIN, OUTPUT);
    setMaxTempValve(false);

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

    if (TempSensors.getDS18Count() > 0 && TempSensors.isConversionComplete())
    {
        digitalWrite(LED_BUILTIN, LED_ON);
        float tempMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);
        tBuffer = (tempMeasured == DEVICE_DISCONNECTED_C)
            ? 0.0
            : tempMeasured + PersistentData.tempOffset;
        TempSensors.requestTemperatures();
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if (!maxTempValveOverride)
    {
        if ((tBuffer > PersistentData.maxTemp) && !maxTempValveActivated)
        {
            setMaxTempValve(true);
        }

        if ((tBuffer < (PersistentData.maxTemp - MAX_TEMP_DELTA_T)) && maxTempValveActivated)
        {
            setMaxTempValve(false);
        }
    }

    delay(10);
}

void onTimeServerSynced()
{
    // Create first buffer log entry
    lastBufferLogEntryPtr = new BufferLogEntry();
    lastBufferLogEntryPtr->time = currentTime - (currentTime % BUFFER_LOG_INTERVAL);
    BufferLog.add(lastBufferLogEntryPtr);
    bufferLogTime = currentTime;
}

void onWiFiInitialized()
{
    if (currentTime > bufferLogTime)
    {
        bufferLogTime++;
        updateBufferLog();
    }
}

void updateBufferLog()
{
    if (currentTime >= lastBufferLogEntryPtr->time + BUFFER_LOG_INTERVAL)
    {
        lastBufferLogEntryPtr = new BufferLogEntry();
        lastBufferLogEntryPtr->time = currentTime;
        BufferLog.add(lastBufferLogEntryPtr);
    }
    lastBufferLogEntryPtr->update(tBuffer, maxTempValveActivated);
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
    else if (message.startsWith("B"))
    {
        float temp = 40;
        for (int i = 0; i < BUFFER_LOG_INTERVAL; i++)
        {
            bool valveActivated = (i % 10) == 0;
            lastBufferLogEntryPtr->update(temp, valveActivated);
            if (temp++ == 100)
                temp = 40;
        }
    }
    else if (message.startsWith("T"))
    {
        tBuffer = message.substring(1).toFloat();
    }
}

float getBarValue(float t, float tMin = 20, float tMax = 100)
{
    return (t - tMin) / (tMax - tMin);
}

void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    if (calibrationRequired)
    {
        handleHttpCalibrateFormRequest();
        return;
    }

    if (WiFiSM.shouldPerformAction(F("valve")))
    {
        // Toggle valve state (through Web UI)
        setMaxTempValve(!maxTempValveActivated);
        maxTempValveOverride = maxTempValveActivated;
    }

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>Buffer Monitor status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th>T<sub>max</sub></th><td>%0.1f °C</td></tr>\r\n"), PersistentData.maxTemp);
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Current values</h1>"));

    HttpResponse.println(F("<table>"));

    HttpResponse.printf(F("<tr><th>T<sub>buffer</sub></th><td>%0.1f °C</td><td class=\"graph\">"), tBuffer);
    Html.writeBar(getBarValue(tBuffer), F("waterBar"), true, false);
    HttpResponse.println(F("</td></tr>"));

    HttpResponse.printf(
        F("<tr><th>Valve</th><td><a href=\"?valve=%u\">%s</a></td><td class=\"graph\"></td></tr>\r\n"),
        currentTime,
        maxTempValveActivated ? "On" : "Off"
        );

    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Last 24 hours</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th rowspan='2'>Valve (s)</th><th colspan='3'>T<sub>buffer</sub> (°C)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th></tr>"));

    BufferLogEntry *logEntryPtr = BufferLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)BUFFER_LOG_INTERVAL);
        float avgTemp = logEntryPtr->sumTemp / seconds;

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%d</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            logEntryPtr->valveActivatedSeconds,
            logEntryPtr->minTemp,
            logEntryPtr->maxTemp,
            avgTemp
            );
        Html.writeBar(getBarValue(avgTemp, 20, PersistentData.maxTemp), F("waterBar"), false, false);
        HttpResponse.println(F("</td></tr>"));

        logEntryPtr = BufferLog.getNextEntry();
    }

    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}

void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));

    Html.writeHeader(F("Calibrate sensors"), true, true);

    if (TempSensors.getDS18Count() < 1)
    {
        HttpResponse.println(F("<h2>Missing temperature sensor</h2>"));
        return;
    }

    float tempMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);

    HttpResponse.println(F("<form action=\"/calibrate\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Sensor</th><th>Measured</th><th>Offset</th><th>Effective</th></tr>"));

    HttpResponse.printf(
        F("<tr><td>T<sub>in</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tempOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
        tempMeasured,
        PersistentData.tempOffset,
        tempMeasured + PersistentData.tempOffset);

    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}

void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    float tempMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);

    PersistentData.tempOffset = WebServer.arg("tempOffset").toFloat();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    calibrationRequired = false;

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

    const char *event = EventLog.getFirstEntry();
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
    Html.writeTextBox(CFG_TZ_OFFSET, F("Timezone offset"), String(PersistentData.timeZoneOffset), 3);
    Html.writeTextBox(CFG_MAX_TEMP, F("Max temperature"), String(PersistentData.maxTemp), 3);
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}

void copyString(const String &input, char *buffer, size_t bufferSize)
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

    PersistentData.timeZoneOffset = WebServer.arg(CFG_TZ_OFFSET).toInt();
    PersistentData.maxTemp = WebServer.arg(CFG_MAX_TEMP).toFloat();

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
