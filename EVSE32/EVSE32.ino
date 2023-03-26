//#define DEBUG_ESP_PORT Serial

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
#include <Navigation.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <BLE.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "CurrentSensor.h"
#include "VoltageSensor.h"
#include "IEC61851ControlPilot.h"
#include "Status.h"
#include "DsmrMonitorClient.h"
#include "DayStatistics.h"
#include "ChargeLogEntry.h"


#define SECONDS_PER_DAY (24 * 3600)
#define WIFI_TIMEOUT_MS 2000
#define HTTP_POLL_INTERVAL 60
#define TEMP_POLL_INTERVAL 10
#define CHARGE_CONTROL_INTERVAL 10
#define CHARGE_LOG_SIZE 150
#define CHARGE_LOG_PAGE_SIZE 50
#define EVENT_LOG_LENGTH 50

#define RELAY_START_PIN 12
#define RELAY_ON_PIN 13
#define CURRENT_SENSE_PIN 34
#define VOLTAGE_SENSE_PIN 32
#define RGB_LED_PIN 17
#define CP_OUTPUT_PIN 15
#define CP_INPUT_PIN 33
#define CP_FEEDBACK_PIN 16
#define TEMP_SENSOR_PIN 14

#define TEMP_LIMIT 60
#define ZERO_CURRENT_THRESHOLD 0.2F
#define LOW_CURRENT_THRESHOLD 0.75F
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

void handleHttpRootRequest();
void handleHttpEventLogRequest();
void handleHttpChargeLogRequest();
void handleHttpBluetoothRequest();
void handleHttpBluetoothFormPost();
void handleHttpSmartMeterRequest();
void handleHttpCalibrateRequest();
void handleHttpConfigFormRequest();
void handleHttpConfigFormPost();

Navigation Nav =
{
    .width = F("8em"),
    .files =
    {
        "Logo.png",
        "styles.css",
        "Bluetooth.svg",
        "Calibrate.svg",
        "Cancel.svg",
        "Confirm.svg",
        "Flash.svg",
        "Home.svg",
        "LogFile.svg",
        "Meter.svg",
        "Settings.svg"
    },
    .menuItems = 
    {
        MenuItem
        {
            .icon = F("Home.svg"),
            .label = F("Home"),
            .urlPath = F(""),
            .handler = handleHttpRootRequest
        },
        MenuItem
        {
            .icon = F("LogFile.svg"),
            .label = F("Event log"),
            .urlPath = F("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = F("Flash.svg"),
            .label = F("Charge log"),
            .urlPath = F("chargelog"),
            .handler = handleHttpChargeLogRequest
        },
        MenuItem
        {
            .icon = F("Bluetooth.svg"),
            .label = F("Bluetooth"),
            .urlPath = F("bt"),
            .handler = handleHttpBluetoothRequest,
            .postHandler = handleHttpBluetoothFormPost
        },
        MenuItem
        {
            .icon = F("Meter.svg"),
            .label = F("Smart Meter"),
            .urlPath = F("dsmr"),
            .handler = handleHttpSmartMeterRequest
        },
        MenuItem
        {
            .icon = F("Calibrate.svg"),
            .label = F("Calibrate"),
            .urlPath = F("calibrate"),
            .handler = handleHttpCalibrateRequest
        },
        MenuItem
        {
            .icon = F("Settings.svg"),
            .label = F("Settings"),
            .urlPath = F("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    }
};

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";

const char* Button = "button";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(WIFI_TIMEOUT_MS);
DsmrMonitorClient SmartMeter(WIFI_TIMEOUT_MS);
BLE Bluetooth;
StringBuilder HttpResponse(8192); // 8KB HTTP response buffer
HtmlWriter Html(HttpResponse, Nav.files[0], Nav.files[1], 60);
Log<const char> EventLog(EVENT_LOG_LENGTH);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
CurrentSensor OutputCurrentSensor(CURRENT_SENSE_PIN);
VoltageSensor OutputVoltageSensor(VOLTAGE_SENSE_PIN);
IEC61851ControlPilot ControlPilot(CP_OUTPUT_PIN, CP_INPUT_PIN, CP_FEEDBACK_PIN);
StatusLED RGBLED(RGB_LED_PIN);
OneWire OneWireBus(TEMP_SENSOR_PIN);
DallasTemperature TempSensors(&OneWireBus);
StaticLog<ChargeLogEntry> ChargeLog(CHARGE_LOG_SIZE);
DayStatistics DayStats;

EVSEState state = EVSEState::Booting;
float temperature = 0;
float energyCharged = 0;
float outputCurrent = 0;
float currentLimit = 0;
bool isRelayActivated = false;
bool isWebAuthorized = false;
bool isMeasuringTemp = false;

time_t currentTime = 0;
time_t tempPollTime = 0;
time_t chargeControlTime = 0;
time_t chargingStartedTime = 0;
time_t chargingFinishedTime = 0;

ChargeLogEntry newChargeLogEntry;
ChargeLogEntry* lastChargeLogEntryPtr = nullptr;

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

    if (!SPIFFS.begin())
        WiFiSM.logEvent(F("Starting SPIFFS failed"));

    Nav.registerHttpHandlers(WebServer);
    WebServer.on("/bt/json", handleHttpBluetoothJsonRequest);
    WebServer.on("/current", handleHttpCurrentRequest);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (!RGBLED.begin())
        setFailure(F("Failed initializing RGB LED"));

    if (!OutputCurrentSensor.begin(PersistentData.currentZero, PersistentData.currentScale))
        setFailure(F("Failed initializing current sensor"));

    if (!OutputVoltageSensor.begin())
        setFailure(F("Failed initializing voltage sensor"));

    pinMode(RELAY_START_PIN, OUTPUT);
    pinMode(RELAY_ON_PIN, OUTPUT);
    setRelay(false);

    if (ControlPilot.begin())
        ControlPilot.calibrate();
    else
        setFailure(F("Failed initializing Control Pilot"));

    if (Bluetooth.begin(PersistentData.hostName))
        Bluetooth.registerBeacons(PersistentData.registeredBeaconCount, PersistentData.registeredBeacons);
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
    if (state == EVSEState::Charging)
        chargingFinishedTime = currentTime;
    setState(EVSEState::Failure);
}


void setUnexpectedControlPilotStatus()
{
    WiFiSM.logEvent(F("Control Pilot: %0.1f V"), ControlPilot.getVoltage());
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

    return true;
}


bool setRelay(bool on)
{
    const char* relayState = on ? "on" : "off";
    Tracer tracer(F(__func__), relayState);

    isRelayActivated = on;

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

    if (OutputVoltageSensor.detectSignal() != isRelayActivated)
    {
        if (state != EVSEState::Failure)
        {
            String message = F("Failed setting relay ");
            message += relayState; 
            setFailure(message);
        }
        return false;
    }

    WiFiSM.logEvent(F("Relay set %s"), relayState);
    return true;
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
            }
            else
                setFailure(F("Self-test failed"));
            break;

        case EVSEState::Ready: // Await vehicle connection
            if (cpStatus == ControlPilotStatus::VehicleDetected)
            {
                isWebAuthorized = false;
                if (Bluetooth.startDiscovery())
                    setState(EVSEState::Authorize);
                else
                    setFailure(F("Bluetooth discovery failed"));
            }
            else if (cpStatus != ControlPilotStatus::Standby)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::Authorize: // Await authorization
            if (isChargingAuthorized())
            {
                if (setRelay(true))
                {
                    chargingStartedTime = 0;
                    chargingFinishedTime = 0;
                    energyCharged = 0;
                    ChargeLog.clear();
                    currentLimit = ControlPilot.setCurrentLimit(determineCurrentLimit());
                    setState(EVSEState::AwaitCharging);
                }
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
                chargeControlTime = currentTime + CHARGE_CONTROL_INTERVAL;
                setState(EVSEState::Charging);
            }
            else if (cpStatus == ControlPilotStatus::Standby)
            {
                if (setRelay(false))
                {
                    ControlPilot.setReady();
                    setState(EVSEState::Ready);
                }
            }
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::Charging:
            if (cpStatus == ControlPilotStatus::VehicleDetected || cpStatus == ControlPilotStatus::Standby)
                stopCharging("vehicle");
            else if (currentTime >= chargeControlTime)
            {
                chargeControlTime += CHARGE_CONTROL_INTERVAL;
                chargeControl();
            }
            break;

        case EVSEState::StopCharging:
            if (cpStatus == ControlPilotStatus::VehicleDetected)
                setState(EVSEState::ChargeCompleted);
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
    if (isWebAuthorized)
    {
        WiFiSM.logEvent(F("Charging authorized through web"));
        return true;
    }

    if (Bluetooth.isDeviceDetected())
    {
        WiFiSM.logEvent(F("Charging authorized through Bluetooth"));
        return true;
    } 

    // If discovery complete, start new one.
    if (Bluetooth.getState() == BluetoothState::DiscoveryComplete)
        Bluetooth.startDiscovery();

    return false;
}


bool stopCharging(const char* cause)
{
    Tracer tracer(F(__func__), cause);

    WiFiSM.logEvent(F("Charging stopped by %s."), cause);

    chargingFinishedTime = currentTime;

    ControlPilot.setOff();
    int timeout = 20;
    do
    {
        OutputCurrentSensor.measure();
        outputCurrent = OutputCurrentSensor.getRMS();
    }
    while (outputCurrent > LOW_CURRENT_THRESHOLD && --timeout > 0);
    if (timeout == 0)
        WiFiSM.logEvent(F("Vehicle keeps drawing current: %0.1f A"), outputCurrent);

    if (!setRelay(false)) return false;    

    ControlPilot.setReady();
    ControlPilot.awaitStatus(ControlPilotStatus::VehicleDetected);

    setState(EVSEState::StopCharging);
    return true;
}


float determineCurrentLimit()
{
    if (!SmartMeter.isInitialized)
        return PersistentData.currentLimit;

    if (!WiFiSM.isConnected())
        return 0;

    if (SmartMeter.requestData() != HTTP_CODE_OK)
    {
        WiFiSM.logEvent(F("Smart Meter: %s"), SmartMeter.getLastError().c_str());
        return 0;
    }

    PhaseData& phase = SmartMeter.getElectricity()[PersistentData.dsmrPhase]; 
    float phaseCurrent = phase.Pdelivered / phase.U; 
    if (state == EVSEState::Charging)
        phaseCurrent = std::max(phaseCurrent - outputCurrent, 0.0F);

    float result = PersistentData.currentLimit - phaseCurrent;

    TRACE(F("Phase current: %0.1f A => Current limit = %0.1f A\n"), phaseCurrent, result);

    return result; 
}


void chargeControl()
{
    Tracer tracer(F(__func__));

    if (!OutputVoltageSensor.detectSignal())
    {
        setFailure(F("Output voltage lost"));
        return;        
    }

    OutputCurrentSensor.measure(); 
    if (OutputCurrentSensor.getSampleCount() > 50)
    {
        outputCurrent = OutputCurrentSensor.getRMS();
        if (outputCurrent > currentLimit * 1.25)
        {
            setFailure(F("Output current too high"));
            return;
        }
        float cl = determineCurrentLimit();
        if (cl > 0) currentLimit = ControlPilot.setCurrentLimit(cl);
    }
    else
        WiFiSM.logEvent(F("Insufficient current samples: %d"), OutputCurrentSensor.getSampleCount());

    energyCharged += outputCurrent * CHARGE_VOLTAGE * CHARGE_CONTROL_INTERVAL; // Ws (J)

    newChargeLogEntry.currentLimit = currentLimit;
    newChargeLogEntry.outputCurrent = outputCurrent;
    newChargeLogEntry.temperature = temperature;
    if (lastChargeLogEntryPtr == nullptr || !newChargeLogEntry.equals(lastChargeLogEntryPtr))
    {
        newChargeLogEntry.time = currentTime;
        lastChargeLogEntryPtr = ChargeLog.add(&newChargeLogEntry);
    }
}


bool selfTest()
{
    Tracer tracer(F(__func__));

    ControlPilot.setOff();
    if (!ControlPilot.awaitStatus(ControlPilotStatus::NoPower))
    {
        WiFiSM.logEvent(F("Control Pilot off: %0.1f V\n"), ControlPilot.getVoltage());
        return false;
    }

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

    if (!setRelay(true)) return false;

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent(F("Output current after relay activation: %0.2f A"), outputCurrent);
        setRelay(false);
        return false;
    }

    if (!setRelay(false)) return false;

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent(F("Output current after relay deactivation: %0.2f A"), outputCurrent);
        return false;
    }

    ControlPilot.setReady();
    if (!ControlPilot.awaitStatus(ControlPilotStatus::Standby))
    {
        WiFiSM.logEvent(F("Control Pilot standby: %0.1f V\n"), ControlPilot.getVoltage());
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
        digitalWrite(LED_BUILTIN, LED_ON);
        isMeasuringTemp = true;
        tempPollTime = currentTime + TEMP_POLL_INTERVAL;
        TempSensors.requestTemperatures();
    }

    if (isMeasuringTemp && TempSensors.isConversionComplete())
    {
        float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);
        if (tMeasured == DEVICE_DISCONNECTED_C)
        {
            if (state != EVSEState::Failure)
                setFailure(F("Temperature sensor disconnected"));
        }
        else if (tMeasured == 85)
            WiFiSM.logEvent(F("Invalid temperature sensor reading"));
        else
        {
            temperature = tMeasured + PersistentData.tempSensorOffset;
            DayStats.update(currentTime, temperature);
            if (temperature >= TEMP_LIMIT && state != EVSEState::Failure)
                setFailure(F("Temperature too high"));
        }
        
        isMeasuringTemp = false;
        digitalWrite(LED_BUILTIN, LED_OFF);
    }
}


void test(String message)
{
    Tracer tracer(F(__func__), message.c_str());

    if (message.startsWith("testF"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
            WiFiSM.logEvent(F("Test entry to fill up the event log."));

        for (int i = 0; i < CHARGE_LOG_SIZE; i++)
        {
            newChargeLogEntry.time = currentTime + i * 60;
            newChargeLogEntry.currentLimit = i % 16;
            newChargeLogEntry.outputCurrent = i % 16;
            newChargeLogEntry.temperature = i % 20 + 10;
            lastChargeLogEntryPtr = ChargeLog.add(&newChargeLogEntry);
        }
    }
    else if (message.startsWith("testB"))
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

    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeTableStart();
    HttpResponse.printf(
        F("<tr><th>WiFi RSSI</th><td>%d dBm</td></tr>\r\n"),
        static_cast<int>(WiFi.RSSI()));

    HttpResponse.printf(
        F("<tr><th>WiFi AP</th><td>%s</td></tr>\r\n"),
        WiFi.BSSIDstr().c_str());

    HttpResponse.printf(
        F("<tr><th>Free Heap</th><td>%u</td></tr>\r\n"),
        ESP.getFreeHeap());

    HttpResponse.printf(
        F("<tr><th>Uptime</th><td>%0.1f days</td></tr>\r\n"),
        float(WiFiSM.getUptime()) / SECONDS_PER_DAY);

    Html.writeRowStart();
    Html.writeHeaderCell(F("EVSE State"));
    Html.writeCell(EVSEStateNames[state]);
    Html.writeRowEnd();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Control Pilot"));
    Html.writeCell(ControlPilot.getStatusName());
    Html.writeRowEnd();

    if (state == EVSEState::Charging)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(F("Current limit"));
        Html.writeCell(currentLimit, F("%0.1f A"));
        Html.writeRowEnd();

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

    Html.writeRowStart();
    Html.writeHeaderCell(F("T<sub>max</sub>"));
    HttpResponse.printf(
        F("<td>%0.1f °C @ %s</td>"),
        DayStats.tMax,
        formatTime("%H:%M", DayStats.tMaxTime));
    Html.writeRowEnd();

    Html.writeRowStart();
    Html.writeHeaderCell(F("T<sub>min</sub>"));
    HttpResponse.printf(
        F("<td>%0.1f °C @ %s</td>"),
        DayStats.tMin,
        formatTime("%H:%M", DayStats.tMinTime));
    Html.writeRowEnd();

    Html.writeTableEnd();

    switch (state)
    {
        case EVSEState::Ready:
        case EVSEState::Failure:
            if (WiFiSM.shouldPerformAction(F("selftest")))
            {
                Html.writeParagraph(F("Performing self-test..."));
                setState(EVSEState::SelfTest);
            }
            else
                Html.writeActionLink(F("selftest"), F("Perform self-test"), currentTime, Button, F("Calibrate.svg"));
            break;

        case EVSEState::Authorize:
            if (WiFiSM.shouldPerformAction(F("authorize")))
            {
                Html.writeParagraph(F("Charging authorized."));
                isWebAuthorized = true;
            }
            else if (!isWebAuthorized)
                Html.writeActionLink(F("authorize"), F("Start charging"), currentTime, Button, F("Flash.svg"));
            break;

        case EVSEState::AwaitCharging:
        case EVSEState::Charging:
            if (WiFiSM.shouldPerformAction(F("stop")))
            {
                if (stopCharging("EVSE"))
                    Html.writeParagraph(F("Charging stopped."));
                else
                    Html.writeParagraph(F("Stop charging failed."));
            }
            else
                Html.writeActionLink(F("stop"), F("Stop charging"), currentTime, Button, F("Cancel.svg"));
            break;
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpBluetoothRequest()
{
    Tracer tracer(F(__func__));

    BluetoothState btState = Bluetooth.getState();
    uint16_t refreshInterval = (btState == BluetoothState::Discovering) ? 5 : 0;

    Html.writeHeader(F("Bluetooth"), Nav, refreshInterval);

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
        Html.writeActionLink(F("startDiscovery"), F("Scan for devices"), currentTime, Button);

    if (Bluetooth.isDeviceDetected())
        Html.writeParagraph(F("Registered device detected"));

    Html.writeFormStart(F("/bt"));

    Html.writeHeading(F("Registered beacons"), 2);
    for (int i = 0; i < PersistentData.registeredBeaconCount; i++)
    {
        UUID128 uuid = UUID128(PersistentData.registeredBeacons[i]);
        String uuidStr = uuid.toString();
        HttpResponse.printf(
            F("<div><input type=\"checkbox\" name=\"uuid\" value=\"%s\" checked>%s</div>\r\n"), 
            uuidStr.c_str(),
            uuidStr.c_str());
    }

    if (btState == BluetoothState::DiscoveryComplete)
    {
        Html.writeHeading(F("Discovered beacons"), 2);
        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell(F(""));
        Html.writeHeaderCell(F("Address"));
        Html.writeHeaderCell(F("UUID"));
        Html.writeHeaderCell(F("RSSI"));
        Html.writeRowEnd();
        for (BluetoothDeviceInfo& btDeviceInfo : Bluetooth.getDiscoveredDevices())
        {
            if (btDeviceInfo.uuid == nullptr) continue;
            const char* uuid = btDeviceInfo.uuid->toString().c_str();

            Html.writeRowStart();
            HttpResponse.printf(
                F("<td><input type=\"checkbox\" name=\"uuid\" value=\"%s\" %s></td>"), 
                uuid,
                btDeviceInfo.isRegistered ? "checked" : "");
            Html.writeCell(btDeviceInfo.getAddress());
            Html.writeCell(uuid);
            Html.writeCell(btDeviceInfo.rssi);
            Html.writeRowEnd();
        }
        Html.writeTableEnd();
    }
    else if (btState == BluetoothState::Discovering)
        HttpResponse.println(F("<p>Discovery in progress...</p>"));

    Html.writeSubmitButton();
    Html.writeFormEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpBluetoothFormPost()
{
    Tracer tracer(F(__func__));

    int n = 0;
    for (int i = 0; i < WebServer.args(); i++)
    {
        if (WebServer.argName(i) == "uuid")
        {
            if (n == MAX_BT_DEVICES) continue;

            String uuidStr = WebServer.arg(i);
            TRACE(F("UUID: '%s'\n"), uuidStr.c_str());

            UUID128 uuid = UUID128(uuidStr);
            memcpy(PersistentData.registeredBeacons[n++], uuid.data, sizeof(uuid128_t));
        }
    }
    
    PersistentData.registeredBeaconCount = n;
    PersistentData.writeToEEPROM();

    Bluetooth.registerBeacons(PersistentData.registeredBeaconCount, PersistentData.registeredBeacons);

    handleHttpBluetoothRequest();
}


void handleHttpBluetoothJsonRequest()
{
    Tracer tracer(F(__func__));

    if (state != EVSEState::Authorize)
    {
        Bluetooth.startDiscovery(2);
        while (Bluetooth.getState() == BluetoothState::Discovering)
        {
            delay(100);
        }
    }

    HttpResponse.clear();
    HttpResponse.print(F("[ "));
    bool first = true;
    for (BluetoothDeviceInfo& btDevice : Bluetooth.getDiscoveredDevices())
    {
        if (first)
            first = false;
        else
            HttpResponse.print(F(", "));

        HttpResponse.printf(
            F("{ \"rssi\": %d, \"bda\": \"%s\", \"name\": \"%s\", \"uuid\": \"%s\", \"manufacturer\": \"%s\", \"isRegistered\": %s }"),
            btDevice.rssi,
            btDevice.getAddress(),
            btDevice.name,
            (btDevice.uuid == nullptr) ? "(N/A)" : btDevice.uuid->toString().c_str(),
            btDevice.getManufacturerName(),
            btDevice.isRegistered ? "true" : "false"
            );
    }
    HttpResponse.println(F(" ]"));

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void handleHttpChargeLogRequest()
{
    Tracer tracer(F(__func__));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((ChargeLog.count() - 1) / CHARGE_LOG_PAGE_SIZE) + 1;

    Html.writeHeader(F("Charge log"), Nav);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    Html.writeHeaderCell(F("I<sub>limit</sub> (A)"));
    Html.writeHeaderCell(F("I<sub>output</sub> (A)"));
    Html.writeHeaderCell(F("T (°C)"));
    Html.writeRowEnd();

    ChargeLogEntry* logEntryPtr = ChargeLog.getFirstEntry();
    for (int i = 0; i < (currentPage * CHARGE_LOG_PAGE_SIZE) && logEntryPtr != nullptr; i++)
    {
        logEntryPtr = ChargeLog.getNextEntry();
    }
    for (int i = 0; i < CHARGE_LOG_PAGE_SIZE && logEntryPtr != nullptr; i++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M:%S", logEntryPtr->time));
        Html.writeCell(logEntryPtr->currentLimit);
        Html.writeCell(logEntryPtr->outputCurrent);
        Html.writeCell(logEntryPtr->temperature);
        Html.writeRowEnd();

        logEntryPtr = ChargeLog.getNextEntry();
    }

    Html.writeTableEnd();
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
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, Button);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSmartMeterRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Smart Meter"), Nav);

    if (SmartMeter.isInitialized)
    {
        int dsmrResult = SmartMeter.requestData();
        if (dsmrResult == HTTP_CODE_OK)
        {
            std::vector<PhaseData> electricity = SmartMeter.getElectricity();

            Html.writeTableStart();
            Html.writeRowStart();
            Html.writeHeaderCell(F("Phase"));
            Html.writeHeaderCell(F("Voltage"));
            Html.writeHeaderCell(F("Current"));
            Html.writeHeaderCell(F("P<sub>delivered</sub>"));
            Html.writeHeaderCell(F("P<sub>returned</sub>"));
            Html.writeRowEnd();
            for (PhaseData& phaseData : electricity)
            {
                Html.writeRowStart();
                Html.writeCell(phaseData.Name);
                Html.writeCell(phaseData.U, F("%0.1f V"));
                Html.writeCell(phaseData.I, F("%0.0f A"));
                Html.writeCell(phaseData.Pdelivered, F("%0.0f W"));
                Html.writeCell(phaseData.Preturned, F("%0.0f W"));
                Html.writeRowEnd();
            }
            Html.writeTableEnd();

            TRACE(F("DSMR phase: %d\n"), PersistentData.dsmrPhase);

            PhaseData& monitoredPhaseData = electricity[PersistentData.dsmrPhase];
            HttpResponse.printf(
                F("<p>Phase '%s' current: %0.1f A</p>\r\n"),
                monitoredPhaseData.Name.c_str(),
                monitoredPhaseData.Pdelivered / CHARGE_VOLTAGE);
        }
        else
            HttpResponse.printf(
                F("<p>%s returned %d: %s</p>\r\n"),
                PersistentData.dsmrMonitor,
                dsmrResult,
                SmartMeter.getLastError().c_str());
    }
    else
        Html.writeParagraph(F("Smart Meter is not enabled."));

    HttpResponse.printf(
        F("<p>Configured current limit: %d A</p>\r\n"),
        static_cast<int>(PersistentData.currentLimit));

    float cl = determineCurrentLimit();
    HttpResponse.printf(F("<p>Effective current limit: %0.1f A</p>\r\n"), cl);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCurrentRequest()
{
    Tracer tracer(F(__func__));

    bool raw = WebServer.hasArg("raw");

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(10);

    HttpResponse.clear();
    OutputCurrentSensor.writeSampleCsv(HttpResponse, raw);

    WebServer.send(200, ContentTypeText, HttpResponse.c_str());
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
    {
        PersistentData.validate();
        PersistentData.writeToEEPROM();
    }

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(50); // Measure 50 periods (1 s) for accuracy

    float outputCurrentRMS = OutputCurrentSensor.getRMS();
    float outputCurrentPeak = OutputCurrentSensor.getPeak();
    float outputCurrentDC = OutputCurrentSensor.getDC();
    float cpVoltage = ControlPilot.getVoltage();
    float cpDutyCycle = ControlPilot.getDutyCycle();

    Html.writeHeader(F("Calibrate"), Nav);

    Html.writeHeading(F("Output current"), 2);
    Html.writeFormStart(F("/calibrate"));
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Samples"));
    Html.writeCell(OutputCurrentSensor.getSampleCount());
    Html.writeRowEnd();
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

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"));
    Html.writeTableStart();
    Html.writeTextBox(
        CFG_WIFI_SSID,
        F("WiFi SSID"),
        PersistentData.wifiSSID,
        sizeof(PersistentData.wifiSSID) - 1);
    Html.writeTextBox(
        CFG_WIFI_KEY, F("WiFi Key"),
        PersistentData.wifiKey,
        sizeof(PersistentData.wifiKey) - 1,
        F("password"));
    Html.writeTextBox(
        CFG_HOST_NAME,
        F("Host name"),
        PersistentData.hostName,
        sizeof(PersistentData.hostName) - 1);
    Html.writeTextBox(
        CFG_NTP_SERVER,
        F("NTP server"),
        PersistentData.ntpServer,
        sizeof(PersistentData.ntpServer) - 1);
    Html.writeTextBox(
        CFG_DSMR_MONITOR,
        F("DSMR monitor"),
        PersistentData.dsmrMonitor,
        sizeof(PersistentData.dsmrMonitor) - 1);
    Html.writeTextBox(
        CFG_DSMR_PHASE,
        F("DSMR phase"),
        String((int)PersistentData.dsmrPhase + 1),
        1,
        F("number"));
    Html.writeTextBox(
        CFG_CURRENT_LIMIT, F("Current limit (A)"),
        String((int)PersistentData.currentLimit),
        2,
        F("number"));
    Html.writeTableEnd();
    Html.writeSubmitButton();
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
        WiFiSM.reset();
    else
        Html.writeActionLink(F("reset"), F("Reset ESP"), currentTime, Button);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
