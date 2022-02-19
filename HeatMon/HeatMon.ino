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
#include <EnergyMeter.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "HeatLogEntry.h"
#include "EnergyLogEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define SPECIFIC_HEAT_CAP_H2O 4.186
#define MAX_FLOW 30
#define MAX_POWER 10
#define MAX_INPUT_POWER 4
#define MAX_COP 5
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
Log<HeatLogEntry> HeatLog(24 * 2); // 24 hrs
Log<EnergyLogEntry> EnergyLog(31); // 31 days
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
FlowSensor Flow_Sensor(D6);
EnergyMeter Energy_Meter(D2);

time_t currentTime = 0;
time_t heatLogTime = 0;
time_t actionPerformedTime = 0;

HeatLogEntry* lastHeatLogEntryPtr = nullptr;
EnergyLogEntry* lastEnergyLogEntryPtr = nullptr;

bool calibrationRequired = false;
float tInput = 0;
float tOutput = 0;
float pOutKW = 0;
float pInKW = 0;

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
            offset
            );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "ERROR: %s sensor is not connected.",
            name
            );
    }

    String event(message);
    logEvent(event);
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
    WebServer.on("/calibrate", HTTP_GET, handleHttpCalibrateFormRequest);
    WebServer.on("/calibrate", HTTP_POST, handleHttpCalibrateFormPost);
    WebServer.on("/heatlog", handleHttpHeatLogRequest);
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

    Flow_Sensor.begin(5.0, 6.6); // 5 sec measure interval, 6.6 Hz @ 1 l/min
    Energy_Meter.begin(100, 1000); // 100 W resolution, 1000 pulses per kWh
    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (!TempSensors.validFamily(PersistentData.tInputSensorAddress))
    {
        // Device addresses not initialized; obtain & store them in persistent data.
        calibrationRequired = true;
        if (!TempSensors.getAddress(PersistentData.tInputSensorAddress, 0))
        {
            logEvent(F("ERROR: Unable to obtain input sensor address."));
            calibrationRequired = false;
        }
        if (!TempSensors.getAddress(PersistentData.tOutputSensorAddress, 1))
        {
            logEvent(F("ERROR: Unable to obtain output sensor address."));
            calibrationRequired = false;
        }
        PersistentData.writeToEEPROM();
    }

    logSensorInfo("Input", PersistentData.tInputSensorAddress, PersistentData.tInputOffset);    
    logSensorInfo("Output", PersistentData.tOutputSensorAddress, PersistentData.tOutputOffset);    

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
        String message = Serial.readStringUntil('\n');
        test(message);
    }

    if (TempSensors.getDS18Count() > 0 && TempSensors.isConversionComplete())
    {
        digitalWrite(LED_BUILTIN, LED_ON);
        float tInputMeasured = TempSensors.getTempC(PersistentData.tInputSensorAddress);
        tInput = (tInputMeasured == DEVICE_DISCONNECTED_C)
            ? 0.0
            : tInputMeasured + PersistentData.tInputOffset;
        if (TempSensors.getDS18Count() > 1)
        {
            float tOutputMeasured = TempSensors.getTempC(PersistentData.tOutputSensorAddress);
            tOutput = (tOutputMeasured == DEVICE_DISCONNECTED_C)
                ? 0.0
                : tOutputMeasured + PersistentData.tOutputOffset;        
        }        
        TempSensors.requestTemperatures();
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    delay(10);
}


void onTimeServerSynced()
{
    // Create first heat log entry
    lastHeatLogEntryPtr = new HeatLogEntry();
    lastHeatLogEntryPtr->time = currentTime - (currentTime % HEAT_LOG_INTERVAL);
    HeatLog.add(lastHeatLogEntryPtr);
    heatLogTime = currentTime;

    // Create first energy log entry (starting 00:00 current day)
    lastEnergyLogEntryPtr = new EnergyLogEntry();
    lastEnergyLogEntryPtr->time = currentTime - (currentTime % SECONDS_PER_DAY);
    EnergyLog.add(lastEnergyLogEntryPtr);
}


void onWiFiInitialized()
{
    if (currentTime >= heatLogTime)
    {
        heatLogTime++;
        updateHeatLog();
        updateEnergyLog();
    }
}


float calcPower(float flowRate, float deltaT)
{
    return SPECIFIC_HEAT_CAP_H2O * (flowRate / 60) * deltaT;
}

void updateHeatLog()
{
    float flowRate = Flow_Sensor.getFlowRate();
    pOutKW = calcPower(flowRate, tInput - tOutput);
    pInKW = Energy_Meter.getPower() / 1000;

    if (currentTime >= lastHeatLogEntryPtr->time + HEAT_LOG_INTERVAL)
    {
        lastHeatLogEntryPtr = new HeatLogEntry();
        lastHeatLogEntryPtr->time = currentTime;
        HeatLog.add(lastHeatLogEntryPtr);
    }
    lastHeatLogEntryPtr->update(tInput, tOutput, flowRate, pOutKW, pInKW);
}

void updateEnergyLog()
{
    if (currentTime >= lastEnergyLogEntryPtr->time + SECONDS_PER_DAY)
    {
        Energy_Meter.resetEnergy();
        lastEnergyLogEntryPtr = new EnergyLogEntry();
        lastEnergyLogEntryPtr->time = currentTime;
        EnergyLog.add(lastEnergyLogEntryPtr);
    }

    lastEnergyLogEntryPtr->energyOut += pOutKW / 3600;
    lastEnergyLogEntryPtr->energyIn = Energy_Meter.getEnergy();
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
            float pOutKW = calcPower(flowRate, tInput - tOutput);
            float pInKw = pOutKW / 4; 
            lastHeatLogEntryPtr->update(tInput, tOutput, flowRate, pOutKW, pInKW);
            if (tInput++ == 60) tInput = 40;
        }
    }
}

float getBarValue(float t, float tMin = 20, float tMax = 60)
{
    return (t - tMin) / (tMax - tMin);
}

float getMaxPower()
{
    float result = 0;
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)HEAT_LOG_INTERVAL);
        result = std::max(result, logEntryPtr->sumPIn / seconds);
        result = std::max(result, logEntryPtr->sumPOut / seconds);
        logEntryPtr = HeatLog.getNextEntry();
    }
    return result;
}

float getMaxEnergy()
{
    float result = 0;
    EnergyLogEntry* logEntryPtr = EnergyLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        result = std::max(result, logEntryPtr->energyIn);
        result = std::max(result, logEntryPtr->energyOut);
        logEntryPtr = EnergyLog.getNextEntry();
    }
    return result;
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

    float flowRate = Flow_Sensor.getFlowRate();
    float cop = (pInKW == 0) ? 0 : pOutKW / pInKW;

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>Heat Monitor status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/heatlog\">Heat log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
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
        F("<tr><th>P<sub>out</sub></th><td>%0.1f kW</td><td class=\"graph\">"),
        pOutKW
        );
    Html.writeBar(pOutKW / MAX_POWER, F("powerBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>P<sub>in</sub></th><td>%0.1f kW</td><td class=\"graph\">"),
        pInKW
        );
    Html.writeBar(pInKW / MAX_INPUT_POWER, F("pInBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.printf(
        F("<tr><th>COP</th><td>%0.1f</td><td class=\"graph\">"),
        cop
        );
    Html.writeBar(cop / MAX_COP, F("copBar"), true, false);
    HttpResponse.println(F("</td></tr>"));
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Energy last 31 days</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Day</th><th>E<sub>out</sub> (kWh)</sub></th><th>E<sub>in</sub> (kWh)</th></tr>"));

    float maxEnergy = std::max(getMaxEnergy(), 1.0f); // Prevent division by zero
    EnergyLogEntry* logEntryPtr = EnergyLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.2f</td><td>%0.2f</td><td class=\"graph\">"),
            formatTime("%d %b", logEntryPtr->time),
            logEntryPtr->energyOut,
            logEntryPtr->energyIn
            );

        Html.writeStackedBar(
            logEntryPtr->energyIn / maxEnergy,
            (logEntryPtr->energyOut - logEntryPtr->energyIn) / maxEnergy,
            F("eInBar"),
            F("energyBar"),
            false,
            false
            );

        HttpResponse.println(F("</td></tr>"));

        logEntryPtr = EnergyLog.getNextEntry();
    }

    HttpResponse.println(F("</table>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}

void handleHttpHeatLogRequest()
{
    Tracer tracer(F("handleHttpHeatLogRequest"));

    Html.writeHeader(F("Heat log"), true, true);

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th colspan='3'>T<sub>in</sub> (°C)</th><th colspan='3'>T<sub>out</sub> (°C)</th><th colspan='3'>Flow rate (l/min)</th><th colspan='3'>P<sub>out</sub> (kW)</th><th colspan='3'>P<sub>in</sub> (kW)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));

    float maxPower = std::max(getMaxPower(), 1.0f); // Prevent division by zero
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)HEAT_LOG_INTERVAL);
        float avgTInput = logEntryPtr->sumTInput / seconds;
        float avgTOutput = logEntryPtr->sumTOutput / seconds;
        float avgFlowRate = logEntryPtr->sumFlowRate / seconds;
        float avgPOut = logEntryPtr->sumPOut / seconds;
        float avgPIn = logEntryPtr->sumPIn / seconds;

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.2f</td><td>%0.1f</td><td>%0.1f</td><td>%0.2f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            logEntryPtr->minTInput,
            logEntryPtr->maxTInput,
            avgTInput,
            logEntryPtr->minTOutput,
            logEntryPtr->maxTOutput,
            avgTOutput,
            logEntryPtr->minFlowRate,
            logEntryPtr->maxFlowRate,
            avgFlowRate,
            logEntryPtr->minPOut,
            logEntryPtr->maxPOut,
            avgPOut,
            logEntryPtr->minPIn,
            logEntryPtr->maxPIn,
            avgPIn
            );

        Html.writeStackedBar(
            avgPIn / maxPower,
            (avgPOut - avgPIn) / maxPower,
            F("pInBar"),
            F("powerBar"),
            false,
            false
            );

        HttpResponse.println(F("</td></tr>"));
        logEntryPtr = HeatLog.getNextEntry();
    }
    HttpResponse.println(F("</table>"));
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}

void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));

    Html.writeHeader(F("Calibrate sensors"), true, true);

    if (TempSensors.getDS18Count() < 2)
    {
        HttpResponse.println(F("<h2>Missing temperature sensors</h2>"));
        HttpResponse.printf(F("<p>Number of temperature sensors detected: %d</p>\r\n"), TempSensors.getDS18Count());
        return;
    }

    HttpResponse.println(F("<p>Ensure that both temperature sensors are measuring the same temperature.</p>"));

    float tInputMeasured = TempSensors.getTempC(PersistentData.tInputSensorAddress);
    float tOutputMeasured = TempSensors.getTempC(PersistentData.tOutputSensorAddress);

    HttpResponse.println(F("<form action=\"/calibrate\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th>Sensor</th><th>Measured</th><th>Offset</th><th>Effective</th></tr>"));

    HttpResponse.printf(
        F("<tr><td>T<sub>in</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tInputOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
        tInputMeasured,
        PersistentData.tInputOffset,
        tInputMeasured + PersistentData.tInputOffset
        );

    HttpResponse.printf(
        F("<tr><td>T<sub>out</sub></td><td>%0.2f °C<td>%0.2f</td><td>%0.2f °C</td></tr>\r\n"),
        tOutputMeasured,
        PersistentData.tOutputOffset,
        tOutputMeasured + PersistentData.tOutputOffset        
        );

    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    float tInputMeasured = TempSensors.getTempC(PersistentData.tInputSensorAddress);
    float tOutputMeasured = TempSensors.getTempC(PersistentData.tOutputSensorAddress);

    PersistentData.tInputOffset = WebServer.arg("tInputOffset").toFloat();
    PersistentData.tOutputOffset = tInputMeasured + PersistentData.tInputOffset - tOutputMeasured;

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
