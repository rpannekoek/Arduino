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
#include "MonitoredTopics.h"
#include "DayStatsEntry.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define SPECIFIC_HEAT_CAP_H2O 4.186
#define MAX_FLOW 30
#define MAX_POWER 10
#define MAX_INPUT_POWER 4
#define HTTP_POLL_INTERVAL 60
#define EVENT_LOG_LENGTH 50
#define BAR_LENGTH 40
#define WIFI_TIMEOUT_MS 2000
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
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, BAR_LENGTH);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StaticLog<HeatLogEntry> HeatLog(24 * 2); // 24 hrs
StaticLog<DayStatsEntry> DayStats(31); // 31 days
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
DayStatsEntry* lastDayStatsEntryPtr = nullptr;

bool newSensorFound = false;
bool maxTempValveActivated = false;
bool maxTempValveOverride = false;

float currentValues[NUMBER_OF_TOPICS];
bool testOverrides[NUMBER_OF_TOPICS];


void logEvent(String msg)
{
    WiFiSM.logEvent(msg);
}


void logSensorInfo(TopicId sensorId)
{
    MonitoredTopic topic = MonitoredTopics[sensorId];
    DeviceAddress& addr = PersistentData.tempSensorAddress[sensorId];

    char message[64];
    if (TempSensors.isConnected(addr))
    {
        snprintf(
            message,
            sizeof(message),
            "%s sensor address: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. Offset: %0.2f",
            topic.label,
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
            PersistentData.tempSensorOffset[sensorId]
            );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "ERROR: %s sensor is not connected.",
            topic.label
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

    for (int i = 0; i < 3; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];

        if ((TempSensors.getDS18Count() > i) && !TempSensors.validFamily(PersistentData.tempSensorAddress[i]))
        {
            newSensorFound = TempSensors.getAddress(PersistentData.tempSensorAddress[i], i);
            if (!newSensorFound)
            {
                String event = F("ERROR: Unable to obtain sensor address for ");
                event += topic.label;
                logEvent(event);
            }
        }

    }

    if (newSensorFound)
        PersistentData.writeToEEPROM();
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
    WebServer.on("/bufferlog", handleHttpBufferLogRequest);
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
    Energy_Meter.begin(100, 1000, 10); // 100 W resolution, 1000 pulses per kWh, max 10 aggregations (=> 6 minutes max)
    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    initTempSensors();

    logSensorInfo(TopicId::TInput);
    logSensorInfo(TopicId::TOutput);
    if (PersistentData.isBufferEnabled())
    {
        logSensorInfo(TopicId::TBuffer);
        pinMode(MAX_TEMP_VALVE_PIN, OUTPUT);
        setMaxTempValve(false);
    }

    TempSensors.requestTemperatures();

    memset(testOverrides, 0, sizeof(testOverrides));

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
        for (int i = 0; i < 3; i++)
        {
            float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress[i]);
            if (tMeasured != DEVICE_DISCONNECTED_C)
                currentValues[i] = tMeasured + PersistentData.tempSensorOffset[i];
        }
        TempSensors.requestTemperatures();
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if (!maxTempValveOverride && (PersistentData.tBufferMax != 0))
    {
        if ((currentValues[TopicId::TBuffer] > PersistentData.tBufferMax) && !maxTempValveActivated)
            setMaxTempValve(true);

        if ((currentValues[TopicId::TBuffer] < (PersistentData.tBufferMax - MAX_TEMP_DELTA_T)) && maxTempValveActivated)
            setMaxTempValve(false);
    }
}


void newHeatLogEntry()
{
    HeatLogEntry newHeatLogEntry;
    newHeatLogEntry.time = currentTime - (currentTime % HEAT_LOG_INTERVAL);
    lastHeatLogEntryPtr = HeatLog.add(&newHeatLogEntry);
}


// Create new energy log entry (starting 00:00 current day)
void newDayStatsEntry()
{
    DayStatsEntry newEntry;
    newEntry.time = currentTime - (currentTime % SECONDS_PER_DAY);
    lastDayStatsEntryPtr = DayStats.add(&newEntry);
}


void onTimeServerSynced()
{
    heatLogTime = currentTime;
    newHeatLogEntry();
    newDayStatsEntry();
}


void onWiFiInitialized()
{
    if (currentTime >= heatLogTime)
    {
        heatLogTime++;
        calculateValues();
        updateHeatLog();
        updateDayStats();
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
        if (DayStats.count() > 1)
        {
            DayStatsEntry* energyLogEntryPtr = DayStats.getEntryFromEnd(2);
            if (energyLogEntryPtr != nullptr)
            {
                dataClient.printf(
                    "%s;%0.1f;%0.1f\r\n",
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


void calculateValues()
{
    if (!testOverrides[TopicId::DeltaT])
        currentValues[TopicId::DeltaT] = currentValues[TopicId::TInput] - currentValues[TopicId::TOutput];

    if (!testOverrides[TopicId::FlowRate])
        currentValues[TopicId::FlowRate] = Flow_Sensor.getFlowRate();

    if (!testOverrides[TopicId::POut])
        currentValues[TopicId::POut] = calcPower(
            currentValues[TopicId::FlowRate],
            currentValues[TopicId::TInput] - currentValues[TopicId::TOutput]);

    if (!testOverrides[TopicId::PIn])
        currentValues[TopicId::PIn] = Energy_Meter.getPower() / 1000;
}


void updateHeatLog()
{
    if (currentTime >= lastHeatLogEntryPtr->time + HEAT_LOG_INTERVAL)
    {
        newHeatLogEntry();
    }

    uint32_t valveSeconds = maxTempValveActivated ? 1 : 0;
    lastHeatLogEntryPtr->update(currentValues, valveSeconds);
}


void updateDayStats()
{
    if (currentTime >= lastDayStatsEntryPtr->time + SECONDS_PER_DAY)
    {
        Energy_Meter.resetEnergy();
        newDayStatsEntry();
        if (PersistentData.isFTPEnabled())
            syncFTPTime = currentTime;
    }

    if (maxTempValveActivated) lastDayStatsEntryPtr->valveActivatedSeconds++;
    lastDayStatsEntryPtr->energyOut += currentValues[TopicId::POut] / 3600;
    lastDayStatsEntryPtr->energyIn = Energy_Meter.getEnergy();
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
        float testValues[NUMBER_OF_TOPICS];
        for (int i = 0; i < 48; i++)
        {
            float tInput = (i % 40) + 25;
            testValues[TopicId::TInput] = tInput;
            testValues[TopicId::TOutput] = tInput - 10;
            testValues[TopicId::TBuffer] = tInput - 5;
            testValues[TopicId::DeltaT] = 10;
            testValues[TopicId::FlowRate] = tInput / 2;
            testValues[TopicId::POut] = calcPower(testValues[TopicId::FlowRate], testValues[TopicId::DeltaT]);
            testValues[TopicId::PIn] = testValues[TopicId::POut] / 4;
            uint32_t valveSeconds = i * 30;
            lastHeatLogEntryPtr->time = currentTime + i * HEAT_LOG_INTERVAL; 
            lastHeatLogEntryPtr->update(testValues, valveSeconds);
            newHeatLogEntry();
        }
    }
    else if (message.startsWith("D"))
    {
        for (int i = 0; i < 31; i++)
        {
            lastDayStatsEntryPtr->energyIn = float(i) / 10;
            lastDayStatsEntryPtr->energyOut = float(i) / 2.5;
            lastDayStatsEntryPtr->valveActivatedSeconds = i * 30;
            lastDayStatsEntryPtr->time = currentTime + i * SECONDS_PER_DAY;
            newDayStatsEntry();
        }
    }
    else if (message.startsWith("T"))
    {
        int eqaulSignIndex = message.indexOf('=');
        if (eqaulSignIndex > 1)
        {
            String topicName = message.substring(1, eqaulSignIndex);
            topicName.trim();

            int topicId = -1;
            for (int i = 0; i < NUMBER_OF_TOPICS; i++)
            {
                if (topicName == MonitoredTopics[i].label)
                {
                    topicId = i;
                    break;
                }
            }
            if (topicId == -1)
                TRACE(F("Unknown topic: '%s'\n"), topicName.c_str());                
            else
            {
                float topicValue = message.substring(eqaulSignIndex + 1).toFloat();
                TRACE(F("Settings topic '%s' (#%d) to %0.1f\n"), topicName.c_str(), topicId, topicValue);
                currentValues[topicId] = topicValue;
                testOverrides[topicId] = true;
            }
        }
        else
            TRACE(F("Unexpected test command:'%s'\n"), message.c_str());
    }
}


float getBarValue(float t, float tMin = 20, float tMax = 60)
{
    return std::max(t - tMin, 0.0F) / (tMax - tMin);
}


float getMaxPower()
{
    float result = 0;
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        result = std::max(result, logEntryPtr->getAverage(TopicId::PIn));
        result = std::max(result, logEntryPtr->getAverage(TopicId::POut));
        logEntryPtr = HeatLog.getNextEntry();
    }
    return result;
}


float getMaxEnergy()
{
    float result = 0;
    DayStatsEntry* logEntryPtr = DayStats.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        result = std::max(result, logEntryPtr->energyIn);
        result = std::max(result, logEntryPtr->energyOut);
        logEntryPtr = DayStats.getNextEntry();
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

    String ftpSync;
    if (PersistentData.ftpServer[0] == 0)
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    Html.writeHeading(F("Heat Monitor status"));
    Html.writeTableStart();
    HttpResponse.printf(F("<tr><th>RSSI</th><td>%d dBm</td></tr>\r\n"), static_cast<int>(WiFi.RSSI()));
    HttpResponse.printf(F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    HttpResponse.printf(F("<tr><th><a href=\"/sync\">FTP Sync</a></th><td>%s</td></tr>\r\n"), ftpSync.c_str());
    HttpResponse.printf(F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td></p>\r\n"), EventLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/heatlog\">Heat log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
    HttpResponse.printf(F("<tr><th><a href=\"/templog\">Temperature log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
    if (PersistentData.isBufferEnabled())
    {
        HttpResponse.printf(F("<tr><th><a href=\"/bufferlog\">Buffer log</a></th><td>%d</td></p>\r\n"), HeatLog.count());
        HttpResponse.printf(
            F("<tr><th>T<sub>buffer,max</sub></th><td>%0.1f °C</td></tr>\r\n"),
            PersistentData.tBufferMax);
        HttpResponse.printf(
            F("<tr><th>T<sub>max</sub> valve</th><td><a href=\"?valve=%u\">%s</a></td></tr>\r\n"),
            (uint32_t)currentTime,
            maxTempValveActivated ? "On" : "Off"
            );
    }
    Html.writeTableEnd();

    writeCurrentValues();
    writeDayStats();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void writeCurrentValues()
{
    float flowRate = Flow_Sensor.getFlowRate();

    Html.writeHeading(F("Current values"));
    Html.writeTableStart();

    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];

        size_t maxBarLength = 0; // Use default bar length
        if (topic.id == TopicId::TBuffer)
        {
            if (!PersistentData.isBufferEnabled()) continue;

            topic.maxValue = PersistentData.tBufferMax;
            maxBarLength = round(PersistentData.tBufferMax - 20); // Ensure 1 °C resolution 
        }

        float topicValue = currentValues[topic.id];
        float barValue = getBarValue(topicValue, topic.minValue, topic.maxValue);
        String barCssClass = topic.style;
        barCssClass += "Bar";

        Html.writeRowStart();
        Html.writeHeaderCell(topic.htmlLabel);
        Html.writeCell(topic.formatValue(topicValue, true));
        Html.writeGraphCell(barValue, barCssClass, true, maxBarLength);
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
}


void writeDayStats()
{
    Html.writeHeading(F("Statistics per day"));

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Day"));
    if (PersistentData.isBufferEnabled())
        Html.writeHeaderCell(F("Valve on"));
    Html.writeHeaderCell(F("E<sub>out</sub> (kWh)"));
    Html.writeHeaderCell(F("E<sub>in</sub> (kWh)"));
    Html.writeHeaderCell(F("COP"));
    Html.writeRowEnd();

    float maxEnergy = std::max(getMaxEnergy(), 0.1F); // Prevent division by zero
    DayStatsEntry* logEntryPtr = DayStats.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%d %b", logEntryPtr->time));
        if (PersistentData.isBufferEnabled())
            Html.writeCell(formatTimeSpan(logEntryPtr->valveActivatedSeconds));
        Html.writeCell(logEntryPtr->energyOut, F("%0.2f"));
        Html.writeCell(logEntryPtr->energyIn, F("%0.2f"));
        Html.writeCell(logEntryPtr->getCOP());
        Html.writeGraphCell(
            logEntryPtr->energyIn / maxEnergy,
            (logEntryPtr->energyOut - logEntryPtr->energyIn) / maxEnergy,
            F("eInBar"),
            F("energyBar"),
            false
            );
        Html.writeRowEnd();

        logEntryPtr = DayStats.getNextEntry();
    }

    Html.writeTableEnd();
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

    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = currentValues[topic.id];
        HttpResponse.printf(F("\"%s\": %s, "), topic.label, topic.formatValue(topicValue, false));
    }

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


void writeMinMaxAvgHeader(int repeat)
{
    Html.writeRowStart();
    for (int i = 0; i < repeat; i++)
    {
        Html.writeHeaderCell(F("Min"));
        Html.writeHeaderCell(F("Max"));
        Html.writeHeaderCell(F("Avg"));
    }
    Html.writeRowEnd();
}


void handleHttpHeatLogRequest()
{
    Tracer tracer(F("handleHttpHeatLogRequest"));

    TopicId showTopicsIds[] = { TopicId::DeltaT, TopicId::FlowRate, TopicId::POut, TopicId::PIn };
    float maxPower = std::max(getMaxPower(), 0.01F); // Prevent division by zero

    Html.writeHeader(F("Heat log"), true, true);

    HttpResponse.printf(
        F("<p>Max: %0.2f kW</p>\r\n"),
        maxPower);

    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("ΔT (°C)"), 3);
    Html.writeHeaderCell(F("Flow (l/min)"), 3);
    Html.writeHeaderCell(F("P<sub>out</sub> (kW)"), 3);
    Html.writeHeaderCell(F("P<sub>in</sub> (kW)"), 3);
    Html.writeRowEnd();

    writeMinMaxAvgHeader(4);

    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        float avgPIn = logEntryPtr->getAverage(TopicId::PIn);
        float avgPOut = logEntryPtr->getAverage(TopicId::POut); 

        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        for (TopicId topicId : showTopicsIds)
        {
            TopicStats topicStats = logEntryPtr->topicStats[topicId];
            Html.writeCell(topicStats.min);
            Html.writeCell(topicStats.max);
            Html.writeCell(logEntryPtr->getAverage(topicId), F("%0.2f"));
        }
        Html.writeGraphCell(
            avgPIn / maxPower,
            (avgPOut - avgPIn) / maxPower,
            F("pInBar"),
            F("powerBar"),
            false
            );
        Html.writeRowEnd();

        logEntryPtr = HeatLog.getNextEntry();
    }
    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpTempLogRequest()
{
    Tracer tracer(F("handleHttpTempLogRequest"));

    TopicId showTopicsIds[] = { TopicId::TInput, TopicId::TOutput };

    Html.writeHeader(F("Temperature log"), true, true);

    HttpResponse.printf(
        F("<p>Min: %0.1f °C. Max: %0.1f °C.</p>\r\n"),
        20.0F,
        60.0F);

    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("T<sub>in</sub> (°C)"), 3);
    Html.writeHeaderCell(F("T<sub>out</sub> (°C)"), 3);
    Html.writeRowEnd();

    writeMinMaxAvgHeader(2);

    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        float avgTInput = logEntryPtr->getAverage(TopicId::TInput);
        float avgTOutput = logEntryPtr->getAverage(TopicId::TOutput);

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        for (TopicId topicId : showTopicsIds)
        {
            TopicStats topicStats = logEntryPtr->topicStats[topicId];
            Html.writeCell(topicStats.min);
            Html.writeCell(topicStats.max);
            Html.writeCell(logEntryPtr->getAverage(topicId));
        }
        Html.writeGraphCell(
            getBarValue(avgTOutput),
            getBarValue(avgTInput) - getBarValue(avgTOutput),
            F("tOutBar"),
            F("waterBar"),
            false
            );
        Html.writeRowEnd();

        logEntryPtr = HeatLog.getNextEntry();
    }
    Html.writeTableEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpBufferLogRequest()
{
    Tracer tracer(F("handleHttpBufferLogRequest"));

    // Auto-ranging: determine min & max buffer temp
    float tMin = 666;
    float tMax = 0;
    HeatLogEntry* logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        float avg = logEntryPtr->getAverage(TopicId::TBuffer);
        tMin = std::min(tMin, avg);
        tMax = std::max(tMax, avg);    
        logEntryPtr = HeatLog.getNextEntry();
    }
    tMax = std::max(tMax, tMin + 1); // Prevent division by zero

    Html.writeHeader(F("Buffer log"), true, true);
    
    HttpResponse.printf(
        F("<p>Min: %0.1f °C. Max: %0.1f °C.</p>\r\n"),
        tMin,
        tMax);

    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("Valve on"), 0, 2);
    Html.writeHeaderCell(F("T<sub>buffer</sub> (°C)"), 3);
    Html.writeRowEnd();

    writeMinMaxAvgHeader(1);

    logEntryPtr = HeatLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        TopicStats topicStats = logEntryPtr->topicStats[TopicId::TBuffer];
        float avgTBuffer = logEntryPtr->getAverage(TopicId::TBuffer);
        float barValue = getBarValue(avgTBuffer, tMin, tMax);

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntryPtr->time));
        Html.writeCell(formatTimeSpan(logEntryPtr->valveActivatedSeconds));
        Html.writeCell(topicStats.min);
        Html.writeCell(topicStats.max);
        Html.writeCell(avgTBuffer);
        Html.writeGraphCell(barValue, F("waterBar"), false);
        Html.writeRowEnd();

        logEntryPtr = HeatLog.getNextEntry();
    }

    Html.writeTableEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));

    Html.writeHeader(F("Calibrate sensors"), true, true);

    if (TempSensors.getDS18Count() < 2)
    {
        Html.writeHeading(F("Missing temperature sensors"), 2);
        HttpResponse.printf(F("<p>Number of temperature sensors detected: %d</p>\r\n"), TempSensors.getDS18Count());
    }
    else
    {
        HttpResponse.println(F("<p>Ensure that all temperature sensors are measuring the same temperature.</p>"));
    
        Html.writeFormStart(F("/calibrate"));

        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell(F("Sensor"));
        Html.writeHeaderCell(F("Measured"));
        Html.writeHeaderCell(F("Offset"));
        Html.writeHeaderCell(F("Effective"));
        Html.writeRowEnd();

        for (int i = 0; i < 3; i++)
        {
            MonitoredTopic topic = MonitoredTopics[i];
            float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress[i]);
            if (tMeasured == DEVICE_DISCONNECTED_C) continue;

            Html.writeRowStart();
            Html.writeCell(topic.htmlLabel);
            Html.writeCell(tMeasured, F("%0.2f °C"));
            HttpResponse.printf(
                F("<td><input type=\"text\" name=\"%s\" value=\"%0.2f\" maxlength=\"5\"></td>"),
                topic.label,
                PersistentData.tempSensorOffset[i]);
            Html.writeCell(tMeasured + PersistentData.tempSensorOffset[i], F("%0.2f °C"));
            Html.writeRowEnd();
        }

        Html.writeTableEnd();    

        Html.writeTableStart();
        Html.writeCheckbox(F("swapInOut"), F("Swap input and output sensors"), false);
        if (PersistentData.isBufferEnabled())
            Html.writeCheckbox(F("swapInBuf"), F("Swap input and buffer sensors"), false);
        Html.writeTableEnd();

        Html.writeSubmitButton();
        Html.writeFormEnd();
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    for (int i = 0; i < 3; i++)
    {
        String argName = MonitoredTopics[i].label;
        if (WebServer.hasArg(argName))
        {
            PersistentData.tempSensorOffset[i] = WebServer.arg(argName).toFloat();
        }
    }

    if (WebServer.hasArg("swapInOut"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tempSensorAddress[TopicId::TInput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TInput], PersistentData.tempSensorAddress[TopicId::TOutput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TOutput], tInputSensorAddress, sizeof(DeviceAddress));
    }
    if (WebServer.hasArg("swapInBuf"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tempSensorAddress[TopicId::TInput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TInput], PersistentData.tempSensorAddress[TopicId::TBuffer], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TBuffer], tInputSensorAddress, sizeof(DeviceAddress));
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

    Html.writeFormStart(F("/config"));
    Html.writeTableStart();
    Html.writeTextBox(CFG_WIFI_SSID, F("WiFi SSID"), PersistentData.wifiSSID, sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(CFG_WIFI_KEY, F("WiFi Key"), PersistentData.wifiKey, sizeof(PersistentData.wifiKey) - 1);
    Html.writeTextBox(CFG_HOST_NAME, F("Host name"), PersistentData.hostName, sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(CFG_NTP_SERVER, F("NTP server"), PersistentData.ntpServer, sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(CFG_FTP_SERVER, F("FTP server"), PersistentData.ftpServer, sizeof(PersistentData.ftpServer) - 1);
    Html.writeTextBox(CFG_FTP_USER, F("FTP user"), PersistentData.ftpUser, sizeof(PersistentData.ftpUser) - 1);
    Html.writeTextBox(CFG_FTP_PASSWORD, F("FTP password"), PersistentData.ftpPassword, sizeof(PersistentData.ftpPassword) - 1);
    Html.writeTextBox(CFG_MAX_TEMP, F("Buffer max"), String(PersistentData.tBufferMax), 3);
    Html.writeTableEnd();
    Html.writeSubmitButton();
    Html.writeFormEnd();

    if (!TempSensors.isConnected(PersistentData.tempSensorAddress[TopicId::TBuffer]))
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
