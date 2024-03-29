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
#include "ChargeStatsEntry.h"

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int HTTP_POLL_INTERVAL = 60;
constexpr int TEMP_POLL_INTERVAL = 10;
constexpr int CHARGE_CONTROL_INTERVAL = 10;
constexpr int CHARGE_LOG_AGGREGATIONS = 6;
constexpr int CHARGE_STATS_SIZE = 5;
constexpr int CHARGE_LOG_SIZE = 200;
constexpr int CHARGE_LOG_PAGE_SIZE = 50;
constexpr int EVENT_LOG_LENGTH = 50;

constexpr uint8_t RELAY_START_PIN = 12;
constexpr uint8_t RELAY_ON_PIN = 13;
constexpr uint8_t CURRENT_SENSE_PIN = 34;
constexpr uint8_t VOLTAGE_SENSE_PIN = 32;
constexpr uint8_t RGB_LED_PIN = 17;
constexpr uint8_t CP_OUTPUT_PIN = 15;
constexpr uint8_t CP_INPUT_PIN = 33;
constexpr uint8_t CP_FEEDBACK_PIN = 16;
constexpr uint8_t TEMP_SENSOR_PIN = 14;

constexpr float ZERO_CURRENT_THRESHOLD = 0.2;
constexpr float LOW_CURRENT_THRESHOLD = 0.75;
constexpr float CHARGE_VOLTAGE = 230;

constexpr uint8_t LED_ON = 0;
constexpr uint8_t LED_OFF = 1;

#define CAL_CURRENT F("ActualCurrent")
#define CAL_CURRENT_ZERO F("CurrentZero")
#define CAL_TEMP_OFFSET F("TempOffset")

enum FileId
{
    Logo,
    Styles,
    BluetoothIcon,
    CalibrateIcon,
    CancelIcon,
    ConfirmIcon,
    FlashIcon,
    HomeIcon,
    LogFileIcon,
    MeterIcon,
    SettingsIcon,
    UploadIcon,
    _LastFileId
};

const char* Files[] PROGMEM =
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
    "Settings.svg",
    "Upload.svg"
};

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";

const char* ButtonClass = "button";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2s timeout
DsmrMonitorClient SmartMeter(5000); // 5s timeout
BLE Bluetooth;
StringBuilder HttpResponse(8192); // 8KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], 60);
Log<const char> EventLog(EVENT_LOG_LENGTH);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
CurrentSensor OutputCurrentSensor(CURRENT_SENSE_PIN);
VoltageSensor OutputVoltageSensor(VOLTAGE_SENSE_PIN);
IEC61851ControlPilot ControlPilot(CP_OUTPUT_PIN, CP_INPUT_PIN, CP_FEEDBACK_PIN);
StatusLED RGBLED(RGB_LED_PIN);
OneWire OneWireBus(TEMP_SENSOR_PIN);
DallasTemperature TempSensors(&OneWireBus);
StaticLog<ChargeLogEntry> ChargeLog(CHARGE_LOG_SIZE);
StaticLog<ChargeStatsEntry> ChargeStats(CHARGE_STATS_SIZE);
DayStatistics DayStats;
Navigation Nav;

EVSEState state = EVSEState::Booting;
float temperature = 0;
float outputCurrent = 0;
float currentLimit = 0;
int aggregations = 0;
bool isRelayActivated = false;
bool isWebAuthorized = false;
bool isMeasuringTemp = false;
bool ftpSyncChargeStats = false;

time_t currentTime = 0;
time_t stateChangeTime = 0;
time_t tempPollTime = 0;
time_t chargeControlTime = 0;
time_t ftpSyncTime = 0;
time_t lastFTPSyncTime = 0;

int logEntriesToSync = 0;
ChargeLogEntry newChargeLogEntry;
ChargeLogEntry* lastChargeLogEntryPtr = nullptr;

ChargeStatsEntry* lastChargeStatsPtr = nullptr;

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

    Nav.width = F("8em");
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
            .urlPath = PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[FlashIcon],
            .label = PSTR("Charge log"),
            .urlPath = PSTR("chargelog"),
            .handler = handleHttpChargeLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
        },
        MenuItem
        {
            .icon = Files[BluetoothIcon],
            .label = PSTR("Bluetooth"),
            .urlPath = PSTR("bt"),
            .handler = handleHttpBluetoothRequest,
            .postHandler = handleHttpBluetoothFormPost
        },
        MenuItem
        {
            .icon = Files[MeterIcon],
            .label = PSTR("Smart Meter"),
            .urlPath = PSTR("dsmr"),
            .handler = handleHttpSmartMeterRequest
        },
        MenuItem
        {
            .icon = Files[CalibrateIcon],
            .label = PSTR("Calibrate"),
            .urlPath = PSTR("calibrate"),
            .handler = handleHttpCalibrateRequest
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath = PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/bt/json", handleHttpBluetoothJsonRequest);
    WebServer.on("/current", handleHttpCurrentRequest);
    WebServer.onNotFound(handleHttpNotFound);
    
    WiFiSM.registerStaticFiles(Files, _LastFileId);
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

    if (!ControlPilot.begin())
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
    stateChangeTime = currentTime;
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
        memset(PersistentData.tempSensorAddress, 0, sizeof(DeviceAddress));
        PersistentData.writeToEEPROM();
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
                ChargeStatsEntry newChargeStats;
                newChargeStats.init(currentTime);
                lastChargeStatsPtr = ChargeStats.add(&newChargeStats);
                newChargeLogEntry.reset(currentTime);
                ChargeLog.clear();
                aggregations = 0;
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
            else if ((currentTime - stateChangeTime) > 60)
                setFailure(F("Timeout waiting for charging to start"));
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
            else if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
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

    if ((PersistentData.authorizeTimeout != 0) && (currentTime - stateChangeTime > PersistentData.authorizeTimeout))
    {
        WiFiSM.logEvent(
            F("Charging authorized by timeout (%s)"),
            formatTimeSpan(PersistentData.authorizeTimeout));
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

    ControlPilot.setOff();
    ControlPilot.awaitStatus(ControlPilotStatus::NoPower);

    // Wait max 5 seconds for output current to drop below threshold
    int timeout = 50;
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

    switch (ControlPilot.getStatus())
    {
        case ControlPilotStatus::Standby:
            setState(EVSEState::Ready);
            break;

        case ControlPilotStatus::VehicleDetected:
            setState(EVSEState::ChargeCompleted);
            break;

        default:
            setState(EVSEState::StopCharging);
    }

    lastChargeStatsPtr->update(currentTime, outputCurrent * CHARGE_VOLTAGE, temperature);
    if (PersistentData.isFTPEnabled())
    {
        ftpSyncTime = currentTime;
        ftpSyncChargeStats = true;
    }

    return true;
}


float getDeratedCurrentLimit()
{
    float result = PersistentData.currentLimit;
    if (temperature > PersistentData.tempLimit)
    {
        // Derate current when above temperature limit: 1 A/degree.
        result = std::min(result, 16.0F) - (temperature - PersistentData.tempLimit);
    }
    return result;
}


float determineCurrentLimit()
{
    float deratedCurrentLimit = getDeratedCurrentLimit();

    if (!SmartMeter.isInitialized)
        return deratedCurrentLimit;

    if (!WiFiSM.isConnected())
        return (temperature > PersistentData.tempLimit) ? deratedCurrentLimit : 0;

    if (SmartMeter.requestData() != HTTP_CODE_OK)
    {
        WiFiSM.logEvent(F("Smart Meter: %s"), SmartMeter.getLastError().c_str());
        return (temperature > PersistentData.tempLimit) ? deratedCurrentLimit : 0;
    }

    PhaseData& phase = SmartMeter.getElectricity()[PersistentData.dsmrPhase - 1]; 
    float phaseCurrent = phase.Pdelivered / phase.U; 
    if (state == EVSEState::Charging)
        phaseCurrent = std::max(phaseCurrent - outputCurrent, 0.0F);

    return std::min((float)PersistentData.currentLimit - phaseCurrent, deratedCurrentLimit);
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

    lastChargeStatsPtr->update(currentTime, outputCurrent * CHARGE_VOLTAGE, temperature);

    newChargeLogEntry.update(currentLimit, outputCurrent, temperature);
    if (++aggregations == CHARGE_LOG_AGGREGATIONS)
    {
        newChargeLogEntry.average(aggregations);
        if (lastChargeLogEntryPtr == nullptr || !newChargeLogEntry.equals(lastChargeLogEntryPtr))
        {
            lastChargeLogEntryPtr = ChargeLog.add(&newChargeLogEntry);

            logEntriesToSync = std::min(logEntriesToSync + 1, CHARGE_LOG_SIZE);
            if (PersistentData.isFTPEnabled() && (logEntriesToSync == PersistentData.ftpSyncEntries))
                ftpSyncTime = currentTime;
        }
        newChargeLogEntry.reset(currentTime);
        aggregations = 0;
    }
}


bool selfTest()
{
    Tracer tracer(F(__func__));

    int cpStandbyLevel = ControlPilot.calibrate();
    WiFiSM.logEvent(F("Control Pilot standby level: %d"), cpStandbyLevel);
    if (cpStandbyLevel < MIN_CP_STANDBY_LEVEL)
    {
        WiFiSM.logEvent(F("Control Pilot standby level too low"));
        return false;
    }

    ControlPilot.setOff();
    if (!ControlPilot.awaitStatus(ControlPilotStatus::NoPower))
    {
        WiFiSM.logEvent(F("Control Pilot off: %0.1f V"), ControlPilot.getVoltage());
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
            if (temperature > (PersistentData.tempLimit + 10) && state != EVSEState::Failure)
                setFailure(F("Temperature too high"));
        }
        
        isMeasuringTemp = false;
        digitalWrite(LED_BUILTIN, LED_OFF);
    }

    if ((ftpSyncTime != 0) && (currentTime >= ftpSyncTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent(F("FTP sync"));
            ftpSyncTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
            ftpSyncTime = currentTime + FTP_RETRY_INTERVAL;
        }
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
        return false;
    }

    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (logEntriesToSync > 0)
        {
            ChargeLogEntry* firstLogEntryPtr = ChargeLog.getEntryFromEnd(logEntriesToSync);
            writeCsvChangeLogEntries(firstLogEntryPtr, dataClient);
            logEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println(F("Nothing to sync."));
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            FTPClient.setUnexpectedResponse();
    }

    if (ftpSyncChargeStats)
    {
        snprintf(filename, sizeof(filename), "%s_stats.csv", PersistentData.hostName);
        if (FTPClient.passive())
        {
            WiFiClient& dataClient = FTPClient.append(filename);
            if (dataClient.connected())
            {
                writeCsvChargeStatsEntry(lastChargeStatsPtr, dataClient);
                dataClient.stop();
            }

            if (FTPClient.readServerResponse() == 226)
                ftpSyncChargeStats = false;
            else
            {
                success = false;
                FTPClient.setUnexpectedResponse();
            }
        }
        else
            success = false;
    }

    FTPClient.end();

    return success;
}


void writeCsvChargeStatsEntry(ChargeStatsEntry* chargeStatsPtr, Print& destination)
{
    destination.printf(
        "%s;%0.1f;%0.1f;%0.1f;%0.1f\r\n",
        formatTime("%F %H:%M", chargeStatsPtr->startTime),
        chargeStatsPtr->getDurationHours(),
        chargeStatsPtr->getAvgTemperature(),
        chargeStatsPtr->getAvgPower() / 1000,
        chargeStatsPtr->energy / 1000);
}


void writeCsvChangeLogEntries(ChargeLogEntry* logEntryPtr, Print& destination)
{
    while (logEntryPtr != nullptr)
    {
        destination.printf(
            "%s;%0.1f;%0.1f;%0.1f\r\n",
            formatTime("%F %H:%M:%S", logEntryPtr->time),
            logEntryPtr->currentLimit,
            logEntryPtr->outputCurrent,
            logEntryPtr->temperature);

        logEntryPtr = ChargeLog.getNextEntry();
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
        logEntriesToSync = 3;

        for (int i = 0; i < CHARGE_STATS_SIZE; i++)
        {
            time_t startTime = currentTime + i * SECONDS_PER_DAY;
            ChargeStatsEntry newChargeStats;
            newChargeStats.init(startTime);
            newChargeStats.update(startTime + (i * SECONDS_PER_HOUR), i * 100, i + 40);
            lastChargeStatsPtr = ChargeStats.add(&newChargeStats);
        }
        ftpSyncChargeStats = true;
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

    Html.writeRow(
        F("EVSE State"),
        F("<span style=\"color: %s; font-weight: bold\">%s</span>"),
        EVSEStateColors[state],
        EVSEStateNames[state]);
    Html.writeRow(F("Control Pilot"), F("%s"), ControlPilot.getStatusName());

    if (state == EVSEState::Charging)
    {
        Html.writeRow(F("Current limit"), F("%0.1f A"), currentLimit);
        Html.writeRow(F("Output current"), F("%0.1f A"), outputCurrent);
    }

    Html.writeRow(F("Temperature"), F("%0.1f °C"), temperature);
    Html.writeRow(F("T<sub>max</sub>"), F("%0.1f °C @ %s"), DayStats.tMax, formatTime("%H:%M", DayStats.tMaxTime));
    Html.writeRow(F("T<sub>min</sub>"), F("%0.1f °C @ %s"), DayStats.tMin, formatTime("%H:%M", DayStats.tMinTime));

    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("WiFi AP"), F("%s"), WiFi.BSSIDstr().c_str());
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeRow(F("FTP Sync"), ftpSync);
    if (PersistentData.isFTPEnabled())
        Html.writeRow(F("Sync entries"), F("%d / %d"), logEntriesToSync, PersistentData.ftpSyncEntries);

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
                Html.writeActionLink(F("selftest"), F("Perform self-test"), currentTime, ButtonClass, Files[CalibrateIcon]);
            break;

        case EVSEState::Authorize:
            if (WiFiSM.shouldPerformAction(F("authorize")))
            {
                Html.writeParagraph(F("Charging authorized."));
                isWebAuthorized = true;
            }
            else if (!isWebAuthorized)
                Html.writeActionLink(F("authorize"), F("Start charging"), currentTime, ButtonClass, Files[FlashIcon]);
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
                Html.writeActionLink(F("stop"), F("Stop charging"), currentTime, ButtonClass, Files[CancelIcon]);
            break;
    }

    writeChargeStatistics();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}

void writeChargeStatistics()
{
    Html.writeHeading("Charging sessions");

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Start");
    Html.writeHeaderCell("Hours");
    Html.writeHeaderCell("T (°C)");
    Html.writeHeaderCell("P (kW)");
    Html.writeHeaderCell("E (kWh)");
    Html.writeRowEnd();

    ChargeStatsEntry* chargeStatsPtr = ChargeStats.getFirstEntry();
    while (chargeStatsPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%d %b %H:%M", chargeStatsPtr->startTime));
        Html.writeCell(chargeStatsPtr->getDurationHours(), F("%0.1f"));
        Html.writeCell(chargeStatsPtr->getAvgTemperature(), F("%0.1f"));
        Html.writeCell(chargeStatsPtr->getAvgPower() / 1000, F("%0.1f"));
        Html.writeCell(chargeStatsPtr->energy / 1000, F("%0.1f"));
        Html.writeRowEnd();

        chargeStatsPtr = ChargeStats.getNextEntry();
    }

    Html.writeTableEnd();
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
        Html.writeActionLink(F("startDiscovery"), F("Scan for devices"), currentTime, ButtonClass);

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
        Html.writeParagraph(F("Discovery in progress..."));

    Html.writeSubmitButton(F("Update registration"), ButtonClass);
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
        Html.writeCell(formatTime("%d %b %H:%M", logEntryPtr->time));
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
        Html.writeDiv(F("%s"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), Nav);

    HttpResponse.println("<pre>");
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println("</pre>");

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        ftpSyncTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());

    Html.writeHeading(F("CSV header"), 2);
    HttpResponse.print(F("<pre>"));
    HttpResponse.println(F("Time;Current Limit;Output Current;Temperature"));
    HttpResponse.println(F("Start;Hours;Temperature;P (kW);E (kWh)"));
    HttpResponse.println(F("</pre>"));

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

            PhaseData& monitoredPhaseData = electricity[PersistentData.dsmrPhase - 1];
            Html.writeParagraph(
                F("Phase '%s' current: %0.1f A"),
                monitoredPhaseData.Name.c_str(),
                monitoredPhaseData.Pdelivered / CHARGE_VOLTAGE);
        }
        else
            Html.writeParagraph(
                F("%s returned %d: %s"),
                PersistentData.dsmrMonitor,
                dsmrResult,
                SmartMeter.getLastError().c_str());
    }
    else
        Html.writeParagraph(F("Smart Meter is not enabled."));

    Html.writeParagraph(
        F("Configured current limit: %d A"),
        static_cast<int>(PersistentData.currentLimit));

    Html.writeParagraph(
        F("Temperature: %0.1f °C => derated current limit: %0.1f A"),
        temperature,
        getDeratedCurrentLimit());

    Html.writeParagraph(
        F("Effective current limit: %0.1f A"),
         determineCurrentLimit());

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
    float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);

    Html.writeHeader(F("Calibrate"), Nav);

    Html.writeHeading(F("Control Pilot"), 2);
    Html.writeTableStart();
    Html.writeRow(F("Measured"), F("%0.2f V"), cpVoltage);
    Html.writeRow(F("Duty Cycle"), F("%0.0f %%"), cpDutyCycle * 100);
    Html.writeTableEnd();

    Html.writeHeading(F("Output current"), 2);
    Html.writeFormStart(F("/calibrate"), F("grid"));
    HttpResponse.printf(F("<label>Samples</label><div>%d</div>\r\n"), OutputCurrentSensor.getSampleCount());
    HttpResponse.printf(F("<label>Measured (DC)</labeL><div>%0.1f mA "), outputCurrentDC * 1000);
    Html.writeActionLink(CAL_CURRENT_ZERO, F("[Calibrate zero]"), currentTime);
    Html.writeDivEnd();
    HttpResponse.printf(F("<label>Measured (Peak)</labeL><div>%0.2f A</div>\r\n"), outputCurrentPeak);
    HttpResponse.printf(F("<label>Measured (RMS)</labeL><div>%0.2f A</div>\r\n"), outputCurrentRMS);
    Html.writeNumberBox(CAL_CURRENT, F("Actual (RMS)"), outputCurrentRMS, 0, 20, 2);
    Html.writeSubmitButton(F("Calibrate"));
    Html.writeFormEnd();

    Html.writeHeading(F("Temperature sensor"), 2);
    Html.writeFormStart(F("/calibrate"), F("grid"));
    HttpResponse.printf(F("<label>Measured</label><div>%0.2f °C</div>\r\n"), tMeasured);
    Html.writeTextBox(CAL_TEMP_OFFSET, F("Offset"), String(PersistentData.tempSensorOffset), 5);
    HttpResponse.printf(F("<label>Effective</label><div>%0.2f °C</div>\r\n"), tMeasured + PersistentData.tempSensorOffset);
    Html.writeSubmitButton(F("Calibrate"));
    Html.writeFormEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"), F("grid"));
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton(F("Save"));
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
        WiFiSM.reset();
    else
        Html.writeActionLink(F("reset"), F("Reset ESP"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F(__func__));

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, ContentTypeText, F("Unexpected request."));
}
