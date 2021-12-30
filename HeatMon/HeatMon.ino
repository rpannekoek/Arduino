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
#include <FlowSensor.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "HeatLogEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define SPECIFIC_HEAT_CAP_H2O 4.186
#define MAX_FLOW 30
#define MAX_POWER 20
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (60 * 60)
#define HEAT_LOG_INTERVAL (30 * 60)

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

const char* ContentTypeHtml = "text/html;charset=UTF-8";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(SECONDS_PER_DAY); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
Log<HeatLogEntry> HeatLog(24 * 2);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
FlowSensor Flow_Sensor(D6);

DeviceAddress tInputSensorAddress;
DeviceAddress tOutputSensorAddress;

time_t currentTime = 0;
time_t heatLogTime = 0;
time_t actionPerformedTime = 0;

HeatLogEntry* lastHeatLogEntryPtr = nullptr;

float tInput = 0;
float tOutput = 0;
float powerKW = 0;

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
    // Turn built-in LED on
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

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

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerInitializing, onTimeServerInit);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Flow_Sensor.begin(5.0, 6.6); // 5 sec measure interval
    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (!TempSensors.getAddress(tInputSensorAddress, 0))
    {
        logEvent(F("ERROR: No input temperature sensor detected."));
    }
    if (!TempSensors.getAddress(tOutputSensorAddress, 1))
    {
        logEvent(F("ERROR: No output temperature sensor detected."));
    }
    TempSensors.requestTemperatures();

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
        digitalWrite(LED_BUILTIN, LED_ON);
        handleSerialData();
        digitalWrite(LED_BUILTIN, LED_OFF);
        return;
    }

    if (TempSensors.getDS18Count() > 0 && TempSensors.isConversionComplete())
    {
        tInput = TempSensors.getTempC(tInputSensorAddress);
        if (TempSensors.getDS18Count() > 1)
        {
            tOutput = TempSensors.getTempC(tOutputSensorAddress);        
        }        
        TempSensors.requestTemperatures();
    }

    delay(10);
}


void onTimeServerInit()
{
}


void onTimeServerSynced()
{
    // Create first global log entry
    lastHeatLogEntryPtr = new HeatLogEntry();
    lastHeatLogEntryPtr->time = currentTime - (currentTime % HEAT_LOG_INTERVAL);
    HeatLog.add(lastHeatLogEntryPtr);
    heatLogTime = currentTime;
}


void onWiFiInitialized()
{
    if (currentTime >= heatLogTime)
    {
        heatLogTime++;
        updateHeatLog();
    }
}


float calcPower(float flowRate, float deltaT)
{
    return SPECIFIC_HEAT_CAP_H2O * (flowRate / 60) * deltaT;
}

void updateHeatLog()
{
    float flowRate = Flow_Sensor.getFlowRate();
    powerKW = calcPower(flowRate, tInput - tOutput);

    if (currentTime >= lastHeatLogEntryPtr->time + HEAT_LOG_INTERVAL)
    {
        lastHeatLogEntryPtr = new HeatLogEntry();
        lastHeatLogEntryPtr->time = currentTime;
        HeatLog.add(lastHeatLogEntryPtr);
    }
    lastHeatLogEntryPtr->update(tInput, tOutput, flowRate, powerKW);
}


void handleSerialData()
{
    Tracer tracer(F("handleSerialData"));

    String message = Serial.readStringUntil('\n');
    test(message);
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
    else if (message.startsWith("H"))
    {
        float tInput = 40;
        for (int i = 0; i < HEAT_LOG_INTERVAL; i++)
        {
            float tOutput = tInput - 10;
            float flowRate = tInput / 2;
            float powerKW = calcPower(flowRate, tInput - tOutput); 
            lastHeatLogEntryPtr->update(tInput, tOutput, flowRate, powerKW);
            if (tInput++ == 60) tInput = 40;
        }
    }
}


bool shouldPerformAction(String name)
{
    if (!WebServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = WebServer.arg(name).toInt();

    if (actionTime == actionPerformedTime)
        return false; // Action already performed

    actionPerformedTime = actionTime;
    return true;
}


float getBarValue(float t, float tMin = 20, float tMax = 60)
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

    float flowRate = Flow_Sensor.getFlowRate();

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>Heat Monitor status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Current values</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>in</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        tInput
        );
    Html.writeBar(getBarValue(tInput), F("waterBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>T<sub>out</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
        tOutput
        );
    Html.writeBar(getBarValue(tOutput), F("waterBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>Delta T</th><td>%0.1f °C</td><td class=\"graph\">"),
        tInput - tOutput
        );
    Html.writeBar(getBarValue(tInput - tOutput, 0, 20), F("waterBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>Flow rate</th><td>%0.1f l/min</td><td class=\"graph\">"),
        flowRate
        );
    Html.writeBar(flowRate / MAX_FLOW, F("flowBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>Power</th><td>%0.1f kW</td><td class=\"graph\">"),
        powerKW
        );
    Html.writeBar(powerKW / MAX_POWER, F("powerBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Last 24 hours</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th colspan='3'>T<sub>in</sub> (°C)</th><th colspan='3'>T<sub>out</sub> (°C)</th><th colspan='3'>Flow rate (l/min)</th><th colspan='3'>Power (kW)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)HEAT_LOG_INTERVAL);
        float avgTInput = logEntryPtr->sumTInput / seconds;
        float avgTOutput = logEntryPtr->sumTOutput / seconds;
        float avgFlow = logEntryPtr->sumFlow / seconds;
        float avgPower = logEntryPtr->sumPower / seconds;

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            logEntryPtr->minTInput,
            logEntryPtr->maxTInput,
            avgTInput,
            logEntryPtr->minTOutput,
            logEntryPtr->maxTOutput,
            avgTOutput,
            logEntryPtr->minFlow,
            logEntryPtr->maxFlow,
            avgFlow,
            logEntryPtr->minPower,
            logEntryPtr->maxPower,
            avgPower
            );

        Html.writeBar(avgPower / MAX_POWER, F("powerBar"), false, false);

        HttpResponse.println(F("</td></tr>"));
        logEntryPtr = HeatLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (shouldPerformAction(F("clear")))
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
    Html.writeTextBox(CFG_TZ_OFFSET, F("Timezone offset"), String(PersistentData.timeZoneOffset), 3);
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

    PersistentData.timeZoneOffset = WebServer.arg(CFG_TZ_OFFSET).toInt();

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
