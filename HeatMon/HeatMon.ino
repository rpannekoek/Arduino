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

#define MAX_TEMP_VALVE_PIN D8
#define MAX_TEMP_DELTA_T 5

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_MAX_TEMP F("tBufferMax")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 40);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<HeatLogEntry> HeatLog(24 * 2); // 24 hrs
StaticLog<EnergyLogEntry> EnergyLog(31); // 31 days
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
FlowSensor Flow_Sensor(D6);
EnergyMeter Energy_Meter(D2);

time_t currentTime = 0;
time_t heatLogTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

HeatLogEntry* lastHeatLogEntryPtr = nullptr;
EnergyLogEntry* lastEnergyLogEntryPtr = nullptr;

bool newSensorFound = false;
bool maxTempValveActivated = false;
bool maxTempValveOverride = false;

float tInput = 0;
float tOutput = 0;
float tBuffer = 0;
float pOutKW = 0;
float pInKW = 0;


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


void setMaxTempValve(bool activated)
{
    maxTempValveActivated = activated;
    digitalWrite(MAX_TEMP_VALVE_PIN, maxTempValveActivated ? 1 : 0);

    if (activated)
        logEvent(F("Max temperature valve set on"));
    else
        logEvent(F("Max temperature valve set off"));
}


void initTempSensors()
{
    Tracer tracer(F("initTempSensors"));

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (TempSensors.getDS18Count() > 0 && !TempSensors.validFamily(PersistentData.tInputSensorAddress))
    {
        newSensorFound = TempSensors.getAddress(PersistentData.tInputSensorAddress, 0);
        if (!newSensorFound)
        {
            logEvent(F("ERROR: Unable to obtain input sensor address."));
        }
    }
    if (TempSensors.getDS18Count() > 1 && !TempSensors.validFamily(PersistentData.tOutputSensorAddress))
    {
        newSensorFound = TempSensors.getAddress(PersistentData.tOutputSensorAddress, 1);
        if (!newSensorFound)
        {
            logEvent(F("ERROR: Unable to obtain output sensor address."));
        }
    }
    if (TempSensors.getDS18Count() > 2 && !TempSensors.validFamily(PersistentData.tBufferSensorAddress))
    {
        if (!TempSensors.getAddress(PersistentData.tBufferSensorAddress, 2))
        {
            logEvent(F("Unable to obtain buffer sensor address."));
        }
        else
            newSensorFound = true;
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
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

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
    WebServer.on("/heatlog", handleHttpHeatLogRequest);
    WebServer.on("/templog", handleHttpTempLogRequest);
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

    initTempSensors();

    logSensorInfo("Input", PersistentData.tInputSensorAddress, PersistentData.tInputOffset);    
    logSensorInfo("Output", PersistentData.tOutputSensorAddress, PersistentData.tOutputOffset);
    if (PersistentData.tBufferMax != 0)
    {
        logSensorInfo("Buffer", PersistentData.tBufferSensorAddress, PersistentData.tBufferOffset);
        pinMode(MAX_TEMP_VALVE_PIN, OUTPUT);
        setMaxTempValve(false);
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
        String message = Serial.readStringUntil('\n');
        test(message);
    }

    if (TempSensors.getDS18Count() > 0 && TempSensors.isConversionComplete())
    {
        digitalWrite(LED_BUILTIN, LED_ON);

        float tInputMeasured = TempSensors.getTempC(PersistentData.tInputSensorAddress);
        if (tInputMeasured != DEVICE_DISCONNECTED_C)
            tInput = tInputMeasured + PersistentData.tInputOffset;

        float tOutputMeasured = TempSensors.getTempC(PersistentData.tOutputSensorAddress);
        if (tOutputMeasured != DEVICE_DISCONNECTED_C)
            tOutput = tOutputMeasured + PersistentData.tOutputOffset;        

        float tBufferMeasured = TempSensors.getTempC(PersistentData.tBufferSensorAddress);
        if (tBufferMeasured != DEVICE_DISCONNECTED_C)
            tBuffer = tBufferMeasured + PersistentData.tBufferOffset;        

        TempSensors.requestTemperatures();
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if (!maxTempValveOverride && (PersistentData.tBufferMax != 0))
    {
        if ((tBuffer > PersistentData.tBufferMax) && !maxTempValveActivated)
            setMaxTempValve(true);

        if ((tBuffer < (PersistentData.tBufferMax - MAX_TEMP_DELTA_T)) && maxTempValveActivated)
            setMaxTempValve(false);
    }

    delay(10);
}


void newHeatLogEntry()
{
    HeatLogEntry newHeatLogEntry;
    newHeatLogEntry.time = currentTime - (currentTime % HEAT_LOG_INTERVAL);
    lastHeatLogEntryPtr = HeatLog.add(&newHeatLogEntry);
}


// Create new energy log entry (starting 00:00 current day)
void newEnergyLogEntry()
{
    EnergyLogEntry newEnergyLogEntry;
    newEnergyLogEntry.time = currentTime - (currentTime % SECONDS_PER_DAY);
    lastEnergyLogEntryPtr = EnergyLog.add(&newEnergyLogEntry);
}


void onTimeServerSynced()
{
    heatLogTime = currentTime;

    // Create first heat log and energy log entries
    newHeatLogEntry();
    newEnergyLogEntry();
}


void onWiFiInitialized()
{
    if (currentTime >= heatLogTime)
    {
        heatLogTime++;
        updateHeatLog();
        updateEnergyLog();
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
        newHeatLogEntry();
    }
    lastHeatLogEntryPtr->update(tInput, tOutput, tBuffer, flowRate, pOutKW, pInKW, maxTempValveActivated);
}


void updateEnergyLog()
{
    if (currentTime >= lastEnergyLogEntryPtr->time + SECONDS_PER_DAY)
    {
        Energy_Meter.resetEnergy();
        newEnergyLogEntry();
        if (PersistentData.ftpServer[0] != 0)
            syncFTPTime = currentTime + (SECONDS_PER_DAY / 2);
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
            float tBuffer = tInput - 5;
            float flowRate = tInput / 2;
            float pOutKW = calcPower(flowRate, tInput - tOutput);
            float pInKw = pOutKW / 4;
            bool valveActivated = (i % 10) == 0; 
            lastHeatLogEntryPtr->update(tInput, tOutput, tBuffer, flowRate, pOutKW, pInKW, valveActivated);
            if (tInput++ == 60) tInput = 40;
        }
    }
    else if (message.startsWith("T"))
    {
        tBuffer = message.substring(1).toFloat();
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

    if (newSensorFound)
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

    float flowRate = Flow_Sensor.getFlowRate();
    float cop = (pInKW == 0) ? 0 : pOutKW / pInKW;

    const char* ftpSync;
    if (PersistentData.ftpServer[0] == 0)
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    HttpResponse.println(F("<h1>Heat Monitor status</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>ESP Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>ESP Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.printf(F("<tr><th><a href=\"/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSync);
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/heatlog\">Heat log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/templog\">Temperature log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
    if (PersistentData.tBufferMax != 0)
    {
        HttpResponse.printf(F("<tr><th>T<sub>buffer,max</sub></th><td>%0.1f °C</td></tr>\r\n"), PersistentData.tBufferMax);
        HttpResponse.printf(
            F("<tr><th>T<sub>max</sub> valve</th><td><a href=\"?valve=%u\">%s</a></td></tr>\r\n"),
            currentTime,
            maxTempValveActivated ? "On" : "Off"
            );
    }
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Current values</h1>"));

    HttpResponse.println(F("<table>"));
    if (PersistentData.tBufferMax != 0)
    {
        HttpResponse.printf(
            F("<tr><th>T<sub>buffer</sub></th><td>%0.1f °C</td><td class=\"graph\">"),
            tBuffer
            );
        size_t maxBarLength = round(PersistentData.tBufferMax - 20); // Ensure 1 °C resolution 
        Html.writeBar(getBarValue(tBuffer, 20, PersistentData.tBufferMax), F("waterBar"), true, false, maxBarLength);
        HttpResponse.println(F("</td></tr>"));
    }
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

    HttpResponse.println(F("<h1>Energy per day</h1>"));

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


void writeJsonFloat(String name, float value)
{
    HttpResponse.printf(F("\"%s\": %0.1f,"), name.c_str(), value);
}

void handleHttpJsonRequest()
{
    Tracer tracer(F("handleHttpJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));
    writeJsonFloat(F("Tin"), tInput);
    writeJsonFloat(F("Tout"), tOutput);
    writeJsonFloat(F("Tbuffer"), tBuffer);
    writeJsonFloat(F("Flow"), Flow_Sensor.getFlowRate());
    writeJsonFloat(F("Pin"), pInKW);
    HttpResponse.printf(F(" \"Valve\": %s }"), maxTempValveActivated ? "true" : "false");

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

void handleHttpHeatLogRequest()
{
    Tracer tracer(F("handleHttpHeatLogRequest"));

    Html.writeHeader(F("Heat log"), true, true);

    HttpResponse.println(F("<table>"));
    HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th colspan='3'>Delta T (°C)</th><th colspan='3'>Flow rate (l/min)</th><th colspan='3'>P<sub>out</sub> (kW)</th><th colspan='3'>P<sub>in</sub> (kW)</th></tr>"));
    HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));

    float maxPower = std::max(getMaxPower(), 1.0f); // Prevent division by zero
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)HEAT_LOG_INTERVAL);
        float avgDeltaT = logEntryPtr->sumDeltaT / seconds;
        float avgFlowRate = logEntryPtr->sumFlowRate / seconds;
        float avgPOut = logEntryPtr->sumPOut / seconds;
        float avgPIn = logEntryPtr->sumPIn / seconds;

        HttpResponse.printf(
            F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.2f</td><td>%0.1f</td><td>%0.1f</td><td>%0.2f</td><td class=\"graph\">"),
            formatTime("%H:%M", logEntryPtr->time),
            logEntryPtr->minDeltaT,
            logEntryPtr->maxDeltaT,
            avgDeltaT,
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


void handleHttpTempLogRequest()
{
    Tracer tracer(F("handleHttpTempLogRequest"));

    Html.writeHeader(F("Temperature log"), true, true);

    HttpResponse.println(F("<table>"));
    bool includeBufferTemp = (PersistentData.tBufferMax != 0); 
    if (includeBufferTemp)
    {
        HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th rowspan='2'>Valve (s)</th><th colspan='3'>T<sub>buffer</sub> (°C)</th><th colspan='3'>T<sub>in</sub> (°C)</th><th colspan='3'>T<sub>out</sub> (°C)</th></tr>"));
        HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));
    }
    else
    {
        HttpResponse.println(F("<tr><th rowspan='2'>Time</th><th colspan='3'>T<sub>in</sub> (°C)</th><th colspan='3'>T<sub>out</sub> (°C)</th></tr>"));
        HttpResponse.println(F("<tr><th>Min</th><th>Max</th><th>Avg</th><th>Min</th><th>Max</th><th>Avg</th></tr>"));
    }

    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        time_t seconds = std::min(currentTime + 1 - logEntryPtr->time, (time_t)HEAT_LOG_INTERVAL);
        float avgTInput = logEntryPtr->sumTInput / seconds;
        float avgTOutput = logEntryPtr->sumTOutput / seconds;

        if (includeBufferTemp)
        {
            float avgTBuffer = logEntryPtr->sumTBuffer / seconds;
            HttpResponse.printf(
                F("<tr><td>%s</td><td>%d</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
                formatTime("%H:%M", logEntryPtr->time),
                logEntryPtr->valveActivatedSeconds,
                logEntryPtr->minTBuffer,
                logEntryPtr->maxTBuffer,
                avgTBuffer,
                logEntryPtr->minTInput,
                logEntryPtr->maxTInput,
                avgTInput,
                logEntryPtr->minTOutput,
                logEntryPtr->maxTOutput,
                avgTOutput
                );
        }
        else
        {
            HttpResponse.printf(
                F("<tr><td>%s</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td>%0.1f</td><td class=\"graph\">"),
                formatTime("%H:%M", logEntryPtr->time),
                logEntryPtr->minTInput,
                logEntryPtr->maxTInput,
                avgTInput,
                logEntryPtr->minTOutput,
                logEntryPtr->maxTOutput,
                avgTOutput
                );
        }

        Html.writeStackedBar(
            getBarValue(avgTOutput),
            (avgTInput - avgTOutput) / 40,
            F("tOutBar"),
            F("waterBar"),
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
    }
    else
    {
        HttpResponse.println(F("<p>Ensure that all temperature sensors are measuring the same temperature.</p>"));
    
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
            F("<tr><td>T<sub>out</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tOutputOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
            tOutputMeasured,
            PersistentData.tOutputOffset,
            tOutputMeasured + PersistentData.tOutputOffset        
            );
    
        if (TempSensors.isConnected(PersistentData.tBufferSensorAddress))
        {
            float tBufferMeasured = TempSensors.getTempC(PersistentData.tBufferSensorAddress);
            HttpResponse.printf(
                F("<tr><td>T<sub>buffer</sub></td><td>%0.2f °C<td><input type=\"text\" name=\"tBufferOffset\" value=\"%0.2f\" maxlength=\"5\"></td><td>%0.2f °C</td></tr>\r\n"),
                tBufferMeasured,
                PersistentData.tBufferOffset,
                tBufferMeasured + PersistentData.tBufferOffset        
                );
        }
    
        HttpResponse.println(F("</table>"));

        Html.writeCheckbox(F("swapInOut"), F("Swap input and output sensors"), false);
        HttpResponse.println(F("<br/>"));
        if (TempSensors.isConnected(PersistentData.tBufferSensorAddress))
        {
            Html.writeCheckbox(F("swapInBuf"), F("Swap input and buffer sensors"), false);
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

    PersistentData.tInputOffset = WebServer.arg("tInputOffset").toFloat();
    PersistentData.tOutputOffset = WebServer.arg("tOutputOffset").toFloat();
    if (WebServer.hasArg("tBufferOffset"))
    {
        PersistentData.tBufferOffset = WebServer.arg("tBufferOffset").toFloat();
    }

    if (WebServer.hasArg("swapInOut"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tInputSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tInputSensorAddress, PersistentData.tOutputSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tOutputSensorAddress, tInputSensorAddress, sizeof(DeviceAddress));
    }
    if (WebServer.hasArg("swapInBuf"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tInputSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tInputSensorAddress, PersistentData.tBufferSensorAddress, sizeof(DeviceAddress));
        memcpy(PersistentData.tBufferSensorAddress, tInputSensorAddress, sizeof(DeviceAddress));
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
    Html.writeTextBox(CFG_MAX_TEMP, F("Buffer max"), String(PersistentData.tBufferMax), 3);
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    if (!TempSensors.isConnected(PersistentData.tBufferSensorAddress))
    {
        HttpResponse.println("<p>NOTE: No buffer sensor is connected. Leave 'Buffer max' zero to suppress buffer temperature in UI.</p>");
    }

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

    PersistentData.tBufferMax = WebServer.arg(CFG_MAX_TEMP).toFloat();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
