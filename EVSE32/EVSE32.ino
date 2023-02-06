#define DEBUG_ESP_PORT Serial

#include <Arduino.h>
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
#include <BLE.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "CurrentSensor.h"
#include "VoltageSensor.h"
#include "IEC61851ControlPilot.h"
#include "Status.h"
#include "DsmrMonitorClient.h"


#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SECONDS_PER_DAY (24 * 3600)
#define WIFI_TIMEOUT_MS 2000
#define HTTP_POLL_INTERVAL 60
#define TEMP_POLL_INTERVAL 6
#define CHARGE_MEASURE_INTERVAL 1.0F
#define EVENT_LOG_LENGTH 50
#define FTP_RETRY_INTERVAL (30 * 60)

#define RELAY_START_PIN 12
#define RELAY_ON_PIN 13
#define CURRENT_SENSE_PIN 34
#define VOLTAGE_SENSE_PIN 32
#define RGB_LED_PIN 17
#define CP_OUTPUT_PIN 15
#define CP_INPUT_PIN 33
#define CP_FEEDBACK_PIN 16
#define TEMP_SENSOR_PIN 14

#define TEMP_TOO_HIGH 60
#define ZERO_CURRENT_THRESHOLD 0.25F
#define CHARGE_VOLTAGE 230

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
#define CFG_DSMR_MONITOR F("DSMRMonitor")
#define CFG_DSMR_PHASE F("DSMRPhase")
#define CFG_CURRENT_LIMIT F("CurrentLimit")
#define CAL_CURRENT F("ActualCurrent")
#define CAL_CURRENT_ZERO F("CurrentZero")
#define CAL_CONTROL_PILOT F("ControlPilot")
#define CAL_TEMP_OFFSET F("TempOffset")

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
DsmrMonitorClient SmartMeter(WIFI_TIMEOUT_MS);
BLE Bluetooth;
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 60);
Log<const char> EventLog(EVENT_LOG_LENGTH);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
CurrentSensor OutputCurrentSensor(CURRENT_SENSE_PIN);
VoltageSensor OutputVoltageSensor(VOLTAGE_SENSE_PIN);
IEC61851ControlPilot ControlPilot(CP_OUTPUT_PIN, CP_INPUT_PIN, CP_FEEDBACK_PIN);
StatusLED RGBLED(RGB_LED_PIN);
OneWire OneWireBus(TEMP_SENSOR_PIN);
DallasTemperature TempSensors(&OneWireBus);
Ticker ChargeControlTicker;

EVSEState state = EVSEState::Booting;
float temperature = 0;
float energyCharged = 0;
float outputCurrent = 0;
float currentLimit = 0;
bool isRelayActivated = false;
bool isWebAuthorized = false;

time_t currentTime = 0;
time_t tempPollTime = 0;
time_t chargingStartedTime = 0;
time_t chargingFinishedTime = 0;


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
    WebServer.on("/bt", HTTP_GET, handleHttpBluetoothRequest);
    WebServer.on("/bt", HTTP_POST, handleHttpBluetoothFormPost);
    WebServer.on("/blejson", handleHttpBLEJsonRequest);
    WebServer.on("/current", handleHttpCurrentRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/calibrate", handleHttpCalibrateRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (!RGBLED.begin())
        setFailure(F("Failed initializing RGB LED"));

    if (!OutputCurrentSensor.begin(PersistentData.currentZero, PersistentData.currentScale))
        setFailure(F("Failed initializing current sensor"));

    if (!OutputVoltageSensor.begin())
        setFailure(F("Failed initializing voltage sensor"));

    pinMode(RELAY_START_PIN, OUTPUT);
    pinMode(RELAY_ON_PIN, OUTPUT);
    if (!setRelay(false))
        setFailure(F("Failed deactivating relay"));

    if (ControlPilot.begin())
        ControlPilot.calibrate();
    else
        setFailure(F("Failed initializing Control Pilot"));

    if (Bluetooth.begin(PersistentData.hostName))
        Bluetooth.registerDevices(PersistentData.registeredDeviceCount, PersistentData.registeredDevices);
    else
        setFailure(F("Failed initializing Bluetooth"));

    if (PersistentData.dsmrMonitor[0] != 0)
    {
        if (!SmartMeter.begin(PersistentData.dsmrMonitor))
            setFailure(F("Failed initializing Smart Meter"));
    }

    initTempSensor();

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


void setState(EVSEState newState)
{
    state = newState;
    WiFiSM.logEvent(F("EVSE State changed to %s"), EVSEStateNames[newState]);
    if (!RGBLED.setStatus(newState))
        WiFiSM.logEvent(F("Failed setting RGB LED status"));
}


void setFailure(const String& reason)
{
    WiFiSM.logEvent(reason);
    if (state != EVSEState::Booting)
    {
        ControlPilot.setOff();
        if (isRelayActivated)
        {
            delay(500);
            setRelay(false);
        } 
    }
    setState(EVSEState::Failure);
}


void setUnexpectedControlPilotStatus()
{
    String message = F("Unexpected Control Pilot status: ");
    message += ControlPilot.getStatusName();
    setFailure(message); 
}


bool initTempSensor()
{
    Tracer tracer(F(__func__));

    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    if (TempSensors.getDS18Count() > 0 && !TempSensors.validFamily(PersistentData.tempSensorAddress))
    {
        bool newSensorFound = TempSensors.getAddress(PersistentData.tempSensorAddress, 0);
        if (newSensorFound)
            PersistentData.writeToEEPROM();
        else
        {
            setFailure(F("Unable to obtain temperature sensor address."));
            return false;
        }
    }

    DeviceAddress& addr = PersistentData.tempSensorAddress;
    if (!TempSensors.isConnected(addr))
    {
        setFailure(F("Temperature sensor is not connected"));
        return false;
    }

    WiFiSM.logEvent(
        F("Temperature sensor address: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. Offset: %0.2f"),
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
        PersistentData.tempSensorOffset);

    TempSensors.requestTemperatures();

    return true;
}


bool setRelay(bool on)
{
    const char* relayState = on ? "on" : "off";
    Tracer tracer(F(__func__), relayState);

    isRelayActivated = on;
    WiFiSM.logEvent(F("Relay set %s"), relayState);

    if (on)
    {
        digitalWrite(RELAY_START_PIN, 1);
        delay(500);
        digitalWrite(RELAY_ON_PIN, 1);
        digitalWrite(RELAY_START_PIN, 0);
    }
    else
    {
        digitalWrite(RELAY_ON_PIN, 0);
        digitalWrite(RELAY_START_PIN, 0);
        delay(100);
    }

    return (OutputVoltageSensor.detectSignal() == on);
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

    runEVSEStateMachine();
}


void runEVSEStateMachine()
{
    ControlPilotStatus cpStatus = ControlPilot.getStatus();

    switch (state)
    {
        case EVSEState::SelfTest:
            if (selfTest())
            {
                WiFiSM.logEvent(F("Self-test passed"));
                setState(EVSEState::Ready);
                ControlPilot.setReady();
            }
            else
                setFailure(F("Self-test failed"));
            break;

        case EVSEState::Ready: // Await vehicle connection
            if (cpStatus == ControlPilotStatus::VehicleDetected)
            {
                isWebAuthorized = false;
                if (Bluetooth.startDiscovery())
                    setState(EVSEState::VehicleDetected);
                else
                    setFailure(F("Bluetooth discovery failed"));
            }
            else if (cpStatus != ControlPilotStatus::Standby)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::VehicleDetected: // Await authorization
            if (isChargingAuthorized())
            {
                if (setRelay(true))
                {
                    chargingStartedTime = 0;
                    chargingFinishedTime = 0;
                    energyCharged = 0;
                    currentLimit = ControlPilot.setCurrentLimit(determineCurrentLimit());
                    setState(EVSEState::AwaitCharging);
                }
                else
                    setFailure(F("Relay activation failed"));
            }
            else if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::AwaitCharging: // Authorized; Wait till vehicle starts charging
            if (cpStatus == ControlPilotStatus::Charging || cpStatus == ControlPilotStatus::ChargingVentilated)
            {
                chargingStartedTime = currentTime;
                ChargeControlTicker.attach(CHARGE_MEASURE_INTERVAL, chargeControl);
                setState(EVSEState::Charging);
            }
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::Charging:
            if (cpStatus == ControlPilotStatus::VehicleDetected || cpStatus == ControlPilotStatus::Standby)
            {
                chargingFinishedTime = currentTime;
                ChargeControlTicker.detach();
                if (setRelay(false))
                    setState(EVSEState::ChargeCompleted);
                else
                    setFailure(F("Relay deactivation failed"));
            }
            else if (cpStatus != ControlPilotStatus::Charging && cpStatus != ControlPilotStatus::ChargingVentilated)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::ChargeCompleted:
            if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;
    }
}


bool isChargingAuthorized()
{
    if (isWebAuthorized || Bluetooth.isDeviceDetected()) return true;

    if (Bluetooth.getState() == BluetoothState::DiscoveryComplete)
        Bluetooth.startDiscovery();

    return false;
}


float determineCurrentLimit()
{
    if (!SmartMeter.isInitialized)
        return PersistentData.currentLimit;

    if (!WiFiSM.isConnected())
        return 0;

    if (SmartMeter.requestData() != HTTP_CODE_OK)
    {
        WiFiSM.logEvent(F("Smart Meter: %s"), SmartMeter.getLastError());
        return 0;
    }

    float phaseCurrent = SmartMeter.phases[PersistentData.dsmrPhase].Pdelivered / CHARGE_VOLTAGE; 
    if (state == EVSEState::Charging)
        phaseCurrent -= OutputCurrentSensor.getRMS();

    float result = PersistentData.currentLimit - phaseCurrent;

    TRACE(F("Phase current: %0.1f A => Current limit = %0.1f A\n"), phaseCurrent, result);

    return result; 
}


void chargeControl()
{
    Tracer tracer(F(__func__));

    if (SmartMeter.isInitialized)
    {
        float cl = determineCurrentLimit();
        if (cl > 0)
            currentLimit = ControlPilot.setCurrentLimit(cl);
    }

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS();

    energyCharged += outputCurrent * CHARGE_VOLTAGE * CHARGE_MEASURE_INTERVAL; // Ws (J)
}


bool selfTest()
{
    Tracer tracer(F(__func__));

    if (OutputVoltageSensor.detectSignal())
    {
        WiFiSM.logEvent(F("Output voltage present before relay activation"));
        return false;
    }

    OutputCurrentSensor.measure();
    float outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent(F("Output current before relay activation: %0.2f A"), outputCurrent);
        return false;
    }

    if (!setRelay(true))
    {
        WiFiSM.logEvent(F("No output voltage after relay activation"));
        setRelay(false);
        return false;
    }

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent(F("Output current after relay activation: %0.2f A"), outputCurrent);
        setRelay(false);
        return false;
    }

    if (!setRelay(false))
    {
        WiFiSM.logEvent(F("Output voltage present after relay deactivation"));
        return false;
    }

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent(F("Output current after relay deactivation: %0.2f A"), outputCurrent);
        return false;
    }

    return true;
}


void onWiFiTimeSynced()
{
    if (state == EVSEState::Booting)
        setState(EVSEState::SelfTest);
    tempPollTime = currentTime;
}


void onWiFiInitialized()
{
    if (currentTime >= tempPollTime && TempSensors.getDS18Count() > 0)
    {
        tempPollTime = currentTime + TEMP_POLL_INTERVAL;
        TempSensors.requestTemperatures();
    }

    if (TempSensors.isConversionComplete() && state != EVSEState::Failure)
    {
        digitalWrite(LED_BUILTIN, LED_ON);

        float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);
        if (tMeasured == DEVICE_DISCONNECTED_C)
            setFailure(F("Temperature sensor disconnected"));
        else if (tMeasured == 85)
            WiFiSM.logEvent(F("Invalid temperature sensor reading"));
        else
        {
            temperature == tMeasured + PersistentData.tempSensorOffset;
            if (temperature >= TEMP_TOO_HIGH)
                setFailure(F("Temperature too high"));
        }
        
        TempSensors.requestTemperatures();

        digitalWrite(LED_BUILTIN, LED_OFF);
    }
}


void test(String message)
{
    Tracer tracer(F(__func__), message.c_str());

    if (message.startsWith("testB"))
    {
        RGBLED.setStatus(EVSEState::Ready); // Color = Breathing Green
    }
    else if (message.startsWith("testL"))
    {
        for (int i = 0; i < 5; i++)
        {
            for (int j = 0; j < 8; j++)
            {
                RGBLED.setStatus(static_cast<EVSEState>(j));
                delay(1000);
            }
        }
    }
    else if (message.startsWith("testR"))
    {
        for (int i = 0; i < 10; i++)
        {
            setRelay(i % 2 == 0);
            delay(5000);
        }
    }
    else if (message.startsWith("testV"))
    {
        if (OutputVoltageSensor.detectSignal(100))
        {
            TRACE(F("Output voltage detected.\n"));
        }
    }
    else if (message.startsWith("testC"))
    {
        TRACE(F("CP Off Voltage: %0.2f V\n"), ControlPilot.getVoltage());

        ControlPilot.setReady();
        delay(10);
        TRACE(F("CP Idle Voltage: %0.2f V\n"), ControlPilot.getVoltage());
        delay(1000);

        for (int i = 20; i >= 5; i--)
        {
            ControlPilot.setCurrentLimit(i);
            delay(500);
            TRACE(F("CP Voltage @ %d A: %0.2f V\n"), i, ControlPilot.getVoltage());
            delay(1500);
        }

        ControlPilot.setReady();
        setState(EVSEState::Ready);
    }
    else if (message.startsWith("s"))
    {
        int number = message.substring(1).toInt();
        setState(static_cast<EVSEState>(number));
    }
}


void handleHttpRootRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    Html.writeHeader(F("Home"), false, false, HTTP_POLL_INTERVAL);

    Html.writeHeading(F("EVSE status"));

    Html.writeTableStart();
    HttpResponse.printf(
        F("<tr><th>RSSI</th><td>%d dBm</td></tr>\r\n"),
        static_cast<int>(WiFi.RSSI()));

    HttpResponse.printf(
        F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"),
        ESP.getFreeHeap());

    HttpResponse.printf(
        F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"),
        float(WiFiSM.getUptime()) / SECONDS_PER_DAY);

    HttpResponse.printf(
        F("<tr><th><a href=\"/events\">Events logged</a></th><td>%d</td>\r\n"),
        EventLog.count());

    HttpResponse.printf(
        F("<tr><th><a href=\"/bt\">Bluetooth</a></th><td>%d / %d</td>\r\n"),
        Bluetooth.getDiscoveredDevices().size(),
        PersistentData.registeredDeviceCount);

    Html.writeRowStart();
    Html.writeHeaderCell(F("EVSE State"));
    Html.writeCell(EVSEStateNames[state]);
    Html.writeRowEnd();

    Html.writeRowStart();
    Html.writeHeaderCell(F("CP Status"));
    Html.writeCell(ControlPilot.getStatusName());
    Html.writeRowEnd();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Current limit"));
    Html.writeCell(currentLimit, F("%0.1f A"));
    Html.writeRowEnd();

    if (state == EVSEState::Charging)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(F("Output current"));
        Html.writeCell(outputCurrent, F("%0.1f A"));
        Html.writeRowEnd();
    }

    Html.writeRowStart();
    Html.writeHeaderCell(F("Energy charged"));
    Html.writeCell(energyCharged / 3600000, F("%0.1f kWh"));
    Html.writeRowEnd();

    if (chargingStartedTime != 0)
    {
        time_t endTime = (chargingFinishedTime == 0) ? currentTime : chargingFinishedTime;

        Html.writeRowStart();
        Html.writeHeaderCell(F("Charge duration"));
        Html.writeCell(formatTimeSpan(endTime - chargingStartedTime));
        Html.writeRowEnd();

        Html.writeRowStart();
        Html.writeHeaderCell(F("Charging started"));
        Html.writeCell(formatTime("%a %H:%M", chargingStartedTime));
        Html.writeRowEnd();
    }

    if (chargingFinishedTime != 0)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(F("Charging finished"));
        Html.writeCell(formatTime("%a %H:%M", chargingFinishedTime));
        Html.writeRowEnd();
    }

    Html.writeRowStart();
    Html.writeHeaderCell(F("Temperature"));
    Html.writeCell(temperature, F("%0.1f °C"));
    Html.writeRowEnd();

    Html.writeTableEnd();

    if (WiFiSM.shouldPerformAction(F("authorize")))
        isWebAuthorized = true;
    else if (state == EVSEState::VehicleDetected && !isWebAuthorized)
        Html.writeActionLink(F("authorize"), F("Start charging"), currentTime);

    if (state == EVSEState::Ready || state == EVSEState::Failure)
    {
        if (WiFiSM.shouldPerformAction(F("selftest")))
        {
            Html.writeParagraph(F("Performing self-test..."));
            setState(EVSEState::SelfTest);
        }
        else
            Html.writeActionLink(F("selftest"), F("Perform self-test"), currentTime);
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpBluetoothRequest()
{
    Tracer tracer(F(__func__));

    BluetoothState btState = Bluetooth.getState();
    uint16_t refreshInterval = (btState == BluetoothState::Discovering) ? 5 : 0;

    Html.writeHeader(F("Bluetooth"), true, true, refreshInterval);

    if (WiFiSM.shouldPerformAction(F("startDiscovery")))
    {
        if (Bluetooth.startDiscovery())
        {
            handleHttpBluetoothRequest();
            return;
        }
        else
            Html.writeParagraph(F("Scanning for devices failed."));
    }
    else if (btState == BluetoothState::Initialized || btState == BluetoothState::DiscoveryComplete)
        Html.writeActionLink(F("startDiscovery"), F("Scan for devices"), currentTime);

    HttpResponse.printf(F("<p>State: %s</p>\r\n"), Bluetooth.getStateName());
    if (Bluetooth.isDeviceDetected())
        Html.writeParagraph(F("Registered device detected"));

    Html.writeHeading(F("Registered devices"), 2);
    for (int i = 0; i < PersistentData.registeredDeviceCount; i++)
    {
        Html.writeParagraph(Bluetooth.formatDeviceAddress(PersistentData.registeredDevices[i]));
    }

    if (btState == BluetoothState::DiscoveryComplete)
    {
        Html.writeHeading(F("Discovered devices"), 2);
        Html.writeFormStart(F("/bt"));
        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell(F(""));
        Html.writeHeaderCell(F("Address"));
        Html.writeHeaderCell(F("Manufacturer"));
        Html.writeHeaderCell(F("Name"));
        Html.writeHeaderCell(F("RSSI"));
        Html.writeRowEnd();
        for (BluetoothDeviceInfo& btDeviceInfo : Bluetooth.getDiscoveredDevices())
        {
            const char* deviceAddress = btDeviceInfo.getAddress();
            Html.writeRowStart();
            HttpResponse.printf(
                F("<td><input type=\"checkbox\" name=\"bda\" value=\"%s\" %s></td>"), 
                deviceAddress,
                PersistentData.isDeviceRegistered(btDeviceInfo.address) ? "checked" : "");
            Html.writeCell(deviceAddress);
            Html.writeCell(btDeviceInfo.getManufacturerName());
            Html.writeCell(btDeviceInfo.name);
            Html.writeCell(btDeviceInfo.rssi);
            Html.writeRowEnd();
        }
        Html.writeTableEnd();
        Html.writeSubmitButton();
        Html.writeFormEnd();
    }
    else if (btState == BluetoothState::Discovering)
        HttpResponse.println(F("<p>Discovery in progress...</p>"));

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpBluetoothFormPost()
{
    Tracer tracer(F(__func__));

    int n = 0;
    for (int i = 0; i < WebServer.args(); i++)
    {
        if (WebServer.argName(i) == "bda")
        {
            if (n == MAX_BT_DEVICES) continue;
            String deviceAddress = WebServer.arg(i);
            TRACE(F("%s\n"), deviceAddress.c_str());
            int bda0, bda1, bda2, bda3, bda4, bda5;
            if (sscanf(deviceAddress.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X", &bda0, &bda1, &bda2, &bda3, &bda4, &bda5) == 6)
            {
                esp_bd_addr_t& bda = PersistentData.registeredDevices[n++];
                bda[0] = bda0;
                bda[1] = bda1;
                bda[2] = bda2;
                bda[3] = bda3;
                bda[4] = bda4;
                bda[5] = bda5;
            }
            else
                WiFiSM.logEvent(F("Invalid BT address: '%s'\n"), deviceAddress.c_str());
        }
    }
    
    PersistentData.registeredDeviceCount = n;
    PersistentData.writeToEEPROM();

    Bluetooth.registerDevices(PersistentData.registeredDeviceCount, PersistentData.registeredDevices);

    handleHttpBluetoothRequest();
}


void handleHttpBLEJsonRequest()
{
    Tracer tracer(F(__func__));

    BLEScan* bleScanPtr = BLEDevice::getScan();
    bleScanPtr->setActiveScan(true); //active scan uses more power, but get results faster
    bleScanPtr->setInterval(333);
    bleScanPtr->setWindow(333);

    BLEScanResults bleScanResult = bleScanPtr->start(1);

    HttpResponse.clear();
    HttpResponse.println(F("["));
    for (int i = 0; i < bleScanResult.getCount(); i++)
    {
        BLEAdvertisedDevice bleDevice = bleScanResult.getDevice(i);
        HttpResponse.printf(
            F("{ \"bda\": \"%s\", \"addrType\": %d, \"name\": \"%s\", \"rssi\": %d, \"manufacturer\": ["),
            bleDevice.getAddress().toString().c_str(),
            bleDevice.getAddressType(),
            bleDevice.getName().c_str(),
            bleDevice.getRSSI()
            );

        const char* manufacturerData = bleDevice.getManufacturerData().data();
        for (int j = 0; j < bleDevice.getManufacturerData().size(); j++)
        {
            if (j > 0) HttpResponse.print(F(", "));
            HttpResponse.printf(F("%d"), (uint8_t)manufacturerData[j]);
        }

        HttpResponse.println(F("] }"));
        if (i < (bleScanResult.getCount() - 1))
            HttpResponse.println(F(","));
        else
            HttpResponse.println();
    }
    HttpResponse.println(F("]"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
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

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse);
}


void handleHttpCurrentRequest()
{
    Tracer tracer(F(__func__));

    bool raw = WebServer.hasArg("raw");

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(10);

    HttpResponse.clear();
    OutputCurrentSensor.writeSampleCsv(HttpResponse, raw);

    WebServer.send(200, ContentTypeText, HttpResponse);
}


void handleHttpCalibrateRequest()
{
    Tracer tracer(F(__func__));

    bool savePersistentData = false;

    if (WiFiSM.shouldPerformAction(CAL_CONTROL_PILOT))
        ControlPilot.calibrate();

    if (WiFiSM.shouldPerformAction(CAL_CURRENT_ZERO))
    {
        PersistentData.currentZero = OutputCurrentSensor.calibrateZero();
        savePersistentData = true;
    }

    if (WebServer.hasArg(CAL_CURRENT))
    {
        float actualCurrentRMS = WebServer.arg(CAL_CURRENT).toFloat();
        PersistentData.currentScale = OutputCurrentSensor.calibrateScale(actualCurrentRMS);
        savePersistentData = true;
    }

    if (WebServer.hasArg(CAL_TEMP_OFFSET))
    {
        PersistentData.tempSensorOffset = WebServer.arg(CAL_TEMP_OFFSET).toFloat();
        savePersistentData = true;
    }

    if (savePersistentData)
        PersistentData.writeToEEPROM();

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(50); // Measure 50 periods (1 s) for accuracy

    float outputCurrentRMS = OutputCurrentSensor.getRMS();
    float outputCurrentPeak = OutputCurrentSensor.getPeak();
    float outputCurrentDC = OutputCurrentSensor.getDC();
    float cpVoltage = ControlPilot.getVoltage();
    float cpDutyCycle = ControlPilot.getDutyCycle();

    Html.writeHeader(F("Calibration"), true, true);

    Html.writeHeading(F("Output current"), 2);
    Html.writeFormStart(F("/calibrate"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Measured (RMS)"));
    Html.writeCell(outputCurrentRMS, F("%0.2f A"));
    Html.writeRowEnd();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Measured (Peak)"));
    Html.writeCell(outputCurrentPeak, F("%0.2f A"));
    Html.writeRowEnd();
    Html.writeHeaderCell(F("Measured (DC)"));
    Html.writeCell(outputCurrentDC * 1000, F("%0.1f mA"));
    Html.writeRowEnd();

    Html.writeTextBox(CAL_CURRENT, F("Actual (RMS)"), String(outputCurrentRMS), 6);
    Html.writeTableEnd();

    Html.writeSubmitButton();
    Html.writeFormEnd();

    if (std::abs(outputCurrentDC) > 0.01)
        Html.writeActionLink(CAL_CURRENT_ZERO, F("Calibrate DC"), currentTime);

    Html.writeHeading(F("Control Pilot"), 2);
    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Measured"));
    Html.writeCell(cpVoltage, F("%0.2f V"));
    Html.writeRowEnd();
    Html.writeHeaderCell(F("Duty Cycle"));
    Html.writeCell(cpDutyCycle * 100, F("%0.0f %%"));
    Html.writeRowEnd();
    Html.writeTableEnd();

    Html.writeActionLink(CAL_CONTROL_PILOT, F("Calibrate Control Pilot Idle"), currentTime);

    Html.writeHeading(F("Temperature sensor"), 2);
    if (TempSensors.isConnected(PersistentData.tempSensorAddress))
    {
        float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);

        Html.writeFormStart(F("/calibrate"));
        Html.writeTableStart();

        Html.writeRowStart();
        Html.writeHeaderCell(F("Measured"));
        Html.writeCell(tMeasured, F("%0.2f °C"));
        Html.writeRowEnd();

        Html.writeTextBox(CAL_TEMP_OFFSET, F("Offset"), String(PersistentData.tempSensorOffset), 5);

        Html.writeRowStart();
        Html.writeHeaderCell(F("Effective"));
        Html.writeCell(tMeasured + PersistentData.tempSensorOffset, F("%0.2f °C"));
        Html.writeRowEnd();

        Html.writeTableEnd();
        Html.writeSubmitButton();
        Html.writeFormEnd();
    }
    else
        Html.writeParagraph(F("Not connected"));

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
    Html.writeTextBox(CFG_DSMR_MONITOR, F("DSMR monitor"), PersistentData.dsmrMonitor, sizeof(PersistentData.dsmrMonitor) - 1);
    Html.writeTextBox(CFG_DSMR_PHASE, F("DSMR phase"), String((int)PersistentData.dsmrPhase + 1), 1);
    Html.writeTextBox(CFG_CURRENT_LIMIT, F("Current limit (A)"), String((int)PersistentData.currentLimit), 2);
    Html.writeTableEnd();
    Html.writeSubmitButton();
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
        WiFiSM.reset();
    else
        Html.writeActionLink(F("reset"), F("Reset ESP"), currentTime);

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
    copyString(WebServer.arg(CFG_DSMR_MONITOR), PersistentData.dsmrMonitor, sizeof(PersistentData.dsmrMonitor)); 

    PersistentData.dsmrPhase = WebServer.arg(CFG_DSMR_PHASE).toInt() - 1;
    PersistentData.currentLimit = WebServer.arg(CFG_CURRENT_LIMIT).toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
