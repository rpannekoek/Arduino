#include <math.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include <Adafruit_VL53L0X.h>
#include "PersistentData.h"
#include "WiFiCredentials.private.h"

#define DEBUG_ESP_PORT Serial
#define MAX_EVENT_LOG_SIZE 100
#define ICON "/apple-touch-icon.png"
#define NTP_SERVER "fritz.box"

#define SCRIPT_TOKEN_SEPARATORS " ,\r\n"
#define MAX_SCRIPT_SIZE 1024
#define MAX_INSTRUCTIONS 256
#define MAX_LOOPS 8

// These pins should not be used (interfere with boot):
// 0, 2, 6-11, 12, 15

#define STEER_LEFT_PIN 16
#define STEER_RIGHT_PIN 17
#define ENGINE_FWD_PIN 18
#define ENGINE_REV_PIN 19

#define FRONT_LIGHTS_PIN 26
#define BRAKE_LIGHTS_PIN 13
#define INDICATOR_LEFT_PIN 14
#define INDICATOR_RIGHT_PIN 27

#define STEER_ENC_BL_PIN 25
#define STEER_ENC_BR_PIN 32
#define STEER_ENC_WB_PIN 33
#define STEER_ENC_WG_PIN 23

#define ENGINE_PWM_CHANNEL 0
#define INDICATOR_PWM_CHANNEL 2

#define ENGINE_PWM_FREQ 50
#define INDICATOR_PWM_FREQ 2

struct Instruction
{
    char command;
    int8_t argument;

    void parse(const char* token)
    {
        command = toupper(token[0]);
        if (strlen(token) > 1)
            argument = abs(min(atoi(token + 1), 255));
        else
            argument = 1;
    }
};

struct LoopInfo
{
    uint16_t startIndex;
    uint16_t endIndex;
    uint16_t count;

    bool isInLoop(uint16_t index)
    {
        return (index >= startIndex) && (index <= endIndex);
    }
};


WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, 24 * 3600); // Synchronize daily
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Adafruit_VL53L0X DistanceSensor = Adafruit_VL53L0X();

volatile VL53L0X_Error lastRangingResult;
VL53L0X_RangingMeasurementData_t lastRangingMeasurement;

int8_t engineSpeed = 0;
int8_t steerPosition = 0;
bool pursuitMode = false;

String script = F("L ( F W B R W B <2 W >2 W ^ W )2 L0");
Instruction instructions[MAX_INSTRUCTIONS];
LoopInfo loops[MAX_LOOPS];
volatile int currentInstructionIndex = -1;
int lastInstructionIndex = -1;
int lastLoopIndex = -1;
volatile bool terminateScript;

TaskHandle_t rangeMonitorTaskHandle = nullptr;
TaskHandle_t scriptRunnerTaskHandle = nullptr;


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
    digitalWrite(LED_BUILTIN, 0);

    Serial.begin(115200);
    Serial.setTimeout(1000);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    TRACE(F("Core ID: %d\n"), xPortGetCoreID());
    #endif

    PersistentData.begin();
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    
    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/script", HTTP_GET, handleHttpScriptRequest);
    WebServer.on("/script", HTTP_POST, handleHttpScriptPostRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    ArduinoOTA.onStart([]() 
        {
            TRACE(F("OTA start %d\n"), ArduinoOTA.getCommand());
            if (ArduinoOTA.getCommand() == U_SPIFFS)
                SPIFFS.end();
        });
    ArduinoOTA.onEnd([]() { TRACE(F("OTA end %d\n"), ArduinoOTA.getCommand()); });
    ArduinoOTA.onError([](ota_error_t error) { TRACE(F("OTA error %u\n"), error); });

    WiFiSM.on(WiFiState::Connected, onWiFiConnected);
    WiFiSM.begin(WIFI_SSID, WIFI_PASSWORD, PersistentData.hostName);

    if (!DistanceSensor.begin(VL53L0X_I2C_ADDR, true))
        TRACE(F("Cannot initialize distance sensor\n"));

    initEngineDrive();
    initSteering();
    initLights();

    spawnRangeMonitor();

    Tracer::traceFreeHeap();

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);
}


void onWiFiConnected()
{
    Tracer tracer(F("onWiFiConnected"));

    ArduinoOTA.begin();

    resetLights();
}


bool spawnRangeMonitor()
{
    Tracer tracer(F("spawnRangeMonitor"));

    xTaskCreatePinnedToCore(
        monitorRange,
        "Range Monitor",
        8192, // Stack Size (words)
        nullptr,
        3, // Priority
        &rangeMonitorTaskHandle,
        PRO_CPU_NUM // Core ID
        );

    return (rangeMonitorTaskHandle != nullptr);
}


bool spawnScriptRunner()
{
    Tracer tracer(F("spawnScriptRunner"));

    if (scriptRunnerTaskHandle != nullptr)
    {
        TRACE(F("scriptRunnerTaskHandle = %p\n"), scriptRunnerTaskHandle);
        logEvent("Cannot start new script: another script is still runnning.");
        return false;
    }

    xTaskCreatePinnedToCore(
        runScript,
        "Script Runner",
        8192, // Stack Size (words)
        nullptr,
        2, // Priority
        &scriptRunnerTaskHandle,
        PRO_CPU_NUM // Core ID
        );

    return (scriptRunnerTaskHandle != nullptr);
}


// Called repeatedly
void loop() 
{
    if (Serial.available())
    {
        digitalWrite(LED_BUILTIN, 0);
        handleSerialRequest();
        digitalWrite(LED_BUILTIN, 1);
    }

    WiFiSM.run();
    ArduinoOTA.handle();

    delay(10);
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");

    static char cmd[32];
    size_t bytesRead = Serial.readBytesUntil('\n', cmd, sizeof(cmd));
    cmd[bytesRead] = 0;

    if (cmd[0] == 'e')
    {
        uint8_t encoderPos = getSteerEncoderPos();
        TRACE("Steer encoder position: %X\n", encoderPos);
    }
    else if (cmd[0] == 's')
    {
        if (parseScript())
            spawnScriptRunner();
    }
    else
    {
        Instruction instruction;
        instruction.parse(cmd);
        if (!executeInstruction(instruction))
            TRACE(F("Unexpected command: %s\n"), cmd);
    }
}


void initLights()
{
    Tracer tracer(F("initLights"));

    pinMode(FRONT_LIGHTS_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(BRAKE_LIGHTS_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(INDICATOR_LEFT_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(INDICATOR_RIGHT_PIN, OUTPUT_OPEN_DRAIN);

    // Turn all lights on
    digitalWrite(FRONT_LIGHTS_PIN, LOW);
    digitalWrite(BRAKE_LIGHTS_PIN, LOW);

    // Initialize indicator PWM
    ledcSetup(INDICATOR_PWM_CHANNEL, INDICATOR_PWM_FREQ, 8);
    ledcWrite(INDICATOR_PWM_CHANNEL, 128);

    // Blink indicator lights
    ledcAttachPin(INDICATOR_LEFT_PIN, INDICATOR_PWM_CHANNEL);
    ledcAttachPin(INDICATOR_RIGHT_PIN, INDICATOR_PWM_CHANNEL);
}


void resetLights()
{
    Tracer tracer(F("resetLights"));

    // Turn all lights off
    digitalWrite(FRONT_LIGHTS_PIN, HIGH);
    digitalWrite(BRAKE_LIGHTS_PIN, HIGH);
    digitalWrite(INDICATOR_LEFT_PIN, HIGH);
    digitalWrite(INDICATOR_RIGHT_PIN, HIGH);

    ledcDetachPin(INDICATOR_LEFT_PIN);
    ledcDetachPin(INDICATOR_RIGHT_PIN);
}


void alarmLights(uint16_t seconds)
{
    Tracer tracer(F("alarmLights"));

    // Blink indicator lights
    ledcAttachPin(INDICATOR_LEFT_PIN, INDICATOR_PWM_CHANNEL);
    ledcAttachPin(INDICATOR_RIGHT_PIN, INDICATOR_PWM_CHANNEL);

    delay(seconds * 1000);

    ledcDetachPin(INDICATOR_LEFT_PIN);
    ledcDetachPin(INDICATOR_RIGHT_PIN);
}


void initEngineDrive()
{
    Tracer tracer(F("initEngineDrive"));

    pinMode(ENGINE_FWD_PIN, OUTPUT);
    pinMode(ENGINE_REV_PIN, OUTPUT);

    digitalWrite(ENGINE_FWD_PIN, LOW);
    digitalWrite(ENGINE_REV_PIN, LOW);

    ledcSetup(ENGINE_PWM_CHANNEL, ENGINE_PWM_FREQ, 8);
}


void initSteering()
{
    Tracer tracer(F("initSteering"));

    pinMode(STEER_LEFT_PIN, OUTPUT);
    pinMode(STEER_RIGHT_PIN, OUTPUT);
    pinMode(STEER_ENC_BL_PIN, INPUT_PULLUP);
    pinMode(STEER_ENC_BR_PIN, INPUT_PULLUP);
    pinMode(STEER_ENC_WB_PIN, INPUT_PULLUP);
    pinMode(STEER_ENC_WG_PIN, INPUT_PULLUP);

    digitalWrite(STEER_LEFT_PIN, LOW);
    digitalWrite(STEER_RIGHT_PIN, LOW);

    centerSteering();
}


void setEngineSpeed(int8_t speed)
{
    Tracer tracer(F("setEngineSpeed"));

    speed = constrain(speed, -2, 5);

    // Linear 8 step: { 0, 32, 64, 96, 128, 160, 192, 224, 255}
    // Exponential 5 step:
    static const uint32_t pwmDutyMap[6] = { 0, 32, 60, 88, 144 , 255};

    uint32_t pwmDuty = pwmDutyMap[abs(speed)];
    TRACE(F("Speed %d => Engine PWM duty: %u\n"), speed, pwmDuty);
    ledcWrite(ENGINE_PWM_CHANNEL, pwmDuty);

    if (speed >= 0)
    {
        digitalWrite(ENGINE_REV_PIN, LOW);
        ledcDetachPin(ENGINE_REV_PIN);
        ledcAttachPin(ENGINE_FWD_PIN, ENGINE_PWM_CHANNEL);
    }
    else
    {
        digitalWrite(ENGINE_FWD_PIN, LOW);
        ledcDetachPin(ENGINE_FWD_PIN);
        ledcAttachPin(ENGINE_REV_PIN, ENGINE_PWM_CHANNEL);
    }

    engineSpeed = speed;
}


void brake(uint16_t seconds)
{
    Tracer tracer(F("brake"));

    digitalWrite(ENGINE_FWD_PIN, HIGH);
    digitalWrite(ENGINE_REV_PIN, HIGH);
    digitalWrite(BRAKE_LIGHTS_PIN, LOW);

    ledcDetachPin(ENGINE_FWD_PIN);
    ledcDetachPin(ENGINE_REV_PIN);

    delay(seconds * 1000);

    digitalWrite(ENGINE_FWD_PIN, LOW);
    digitalWrite(ENGINE_REV_PIN, LOW);
    digitalWrite(BRAKE_LIGHTS_PIN, HIGH);

    engineSpeed = 0;
}


void steer(int8_t toPosition)
{
    Tracer tracer(F("steer"));

    toPosition = constrain(toPosition, -3, 3);
    TRACE(F("toPosition = %d\n"), toPosition);

    if (toPosition == 0)
    {
        // Turn indicator lights off
        ledcDetachPin(INDICATOR_RIGHT_PIN);
        ledcDetachPin(INDICATOR_LEFT_PIN);
    }
    else
    {
        // Turn indicator lights on
        if (toPosition > 0)
        {
            ledcAttachPin(INDICATOR_RIGHT_PIN, INDICATOR_PWM_CHANNEL);
            ledcDetachPin(INDICATOR_LEFT_PIN);
        }
        else
        {
            ledcAttachPin(INDICATOR_LEFT_PIN, INDICATOR_PWM_CHANNEL);
            ledcDetachPin(INDICATOR_RIGHT_PIN);
        }
    }

    if (toPosition == steerPosition)
        return; // Already there

    if (toPosition == 0)
        centerSteering();
    else
    {
        int direction = (toPosition > steerPosition) ? 1 : -1;
        steerToPosition(direction, toPosition * 2);
    }

    steerPosition = toPosition;
}


bool centerSteering()
{
    Tracer tracer(F("centerSteering"));

    int direction = (getSteerEncoderPos() & 8) ? -1 : 1;
    steerToPosition(direction, direction);
    return steerToPosition(-direction, 0);
}


bool steerToPosition(int direction, int8_t targetSteerPos)
{
    Tracer tracer(F("steerToPosition"));

    static uint8_t steerEncoderPos[15] = {0x1, 0x3, 0x2, 0x6, 0x4, 0x5, 0x7, 0xF, 0xD, 0xC, 0xE, 0xA, 0xB, 0x9, 0x8};
    uint8_t targetEncoderPos1 = steerEncoderPos[7 + targetSteerPos];
    uint8_t targetEncoderPos2 = steerEncoderPos[7 + targetSteerPos + direction];
    uint8_t maxEncoderPos = (direction > 0) ? steerEncoderPos[15] : steerEncoderPos[0];
    TRACE(F("Direction: %d. Target pos: %X / %X. Max pos: %X\n"), direction, targetEncoderPos1, targetEncoderPos2, maxEncoderPos);

    uint8_t currentEncoderPos = getSteerEncoderPos();
    TRACE(F("Current pos: %X\n"), currentEncoderPos);

    int i = 0;
    while ((currentEncoderPos != targetEncoderPos1) && (currentEncoderPos != targetEncoderPos2))
    {
        if ((currentEncoderPos == maxEncoderPos) || (i++ == 20))
        {
            TRACE(F("ERROR: max steering reached (%d steps)\n"), i);
            return false;
        }

        currentEncoderPos = moveSteering(direction, 20);
    }

    return true;
}


uint8_t moveSteering(int8_t direction, uint8_t duration)
{
    Tracer tracer(F("moveSteering"));

    // Steering motor on (left/right)
    if (direction > 0)
    {
        digitalWrite(STEER_RIGHT_PIN, HIGH);
        digitalWrite(STEER_LEFT_PIN, LOW);
    }
    else
    {
        digitalWrite(STEER_LEFT_PIN, HIGH);
        digitalWrite(STEER_RIGHT_PIN, LOW);
    }

    delay(duration);

    // Brake steering motor
    digitalWrite(STEER_LEFT_PIN, HIGH);
    digitalWrite(STEER_RIGHT_PIN, HIGH);

    delay(duration * 2);

    // Steering motor off
    digitalWrite(STEER_LEFT_PIN, LOW);
    digitalWrite(STEER_RIGHT_PIN, LOW);

    uint8_t result = getSteerEncoderPos();
    TRACE(F("Steer encoder position: %X\n"), result);
    return result;
}


uint8_t getSteerEncoderPos()
{
    return 
        digitalRead(STEER_ENC_WG_PIN) |
        (digitalRead(STEER_ENC_WB_PIN) << 1) |
        (digitalRead(STEER_ENC_BR_PIN) << 2) |
        (digitalRead(STEER_ENC_BL_PIN) << 3);
}


void monitorRange(void* taskParams)
{
    Tracer tracer(F("monitorRange"));
    TRACE(F("Core ID: %d\n"), xPortGetCoreID());

    char event[64];

    while (true)
    {
        lastRangingResult = DistanceSensor.rangingTest(&lastRangingMeasurement);

        if ((engineSpeed <= 0)  && !pursuitMode)
        {
            // Only monitor during forward motion (unless in pursuit mode)
            delay(100);
            continue;
        }

        if (lastRangingResult != VL53L0X_ERROR_NONE)
        {
            snprintf(event, sizeof(event), "Ranging error: %d", lastRangingResult);
            logEvent(event);
            terminateScript = true; 
            brake(1);
            continue;
        }

        // Collision detect range (mm) as a function of engine speed
        // Note: max range of distance sensor is approx. 1200 mm.
        static uint16_t speedRangeMap[6] = { 0, 100, 400, 700, 1300, 2500 };

        uint16_t collisionDetectRange = speedRangeMap[max<int>(engineSpeed, 0)];
        uint16_t distance = lastRangingMeasurement.RangeMilliMeter;
        if (pursuitMode)
        {
            if (distance < 1200)
            {
                uint16_t slowDownDistance = collisionDetectRange + 100;
                uint16_t speedUpDistance = speedRangeMap[engineSpeed + 1] + 150;
                if ((engineSpeed >= 0) && (distance <= slowDownDistance))
                {
                    TRACE(F("Pursuit slow down; distance below %d mm: %d mm\n"), slowDownDistance, distance);
                    setEngineSpeed(engineSpeed - 1);
                }
                else if ((engineSpeed < 3) && (distance >= speedUpDistance) && (lastRangingMeasurement.RangeStatus != 4))
                {
                    TRACE(F("Pursuit speed up; distance above %d mm: %d mm\n"), speedUpDistance, distance);
                    setEngineSpeed(engineSpeed + 1);
                }
            }
            else if (engineSpeed != 0)
            {
                TRACE(F("Pursuit stop; out of range (%d mm)\n"), distance);
                brake(1);
            }
        }
        else if (distance <= collisionDetectRange)
        {
            if (lastRangingMeasurement.RangeStatus == 0)
            {
                // Reliable measurement
                snprintf(event, sizeof(event), "Emergency stop; collision in %d mm.", distance);
                logEvent(event);
                terminateScript = true;
                if (engineSpeed > 1)
                {
                    // Shortly reverse for more braking power
                    setEngineSpeed(-2);
                    delay(500); 
                }
                brake(2);
            }
            else if (engineSpeed > 2)
            {
                // Less reliable measurement
                snprintf(event, sizeof(event), "Safety mode; potential collision in %d mm.", distance);
                logEvent(event);
                setEngineSpeed(2);
            }
        }
    }
}


bool executeInstruction(Instruction& instruction)
{
    Tracer tracer(F("executeInstruction"));
    TRACE(F("%c: %d\n"), instruction.command, instruction.argument);

    switch (instruction.command)
    {
        case 'F':
            setEngineSpeed(instruction.argument);
            break;

        case 'R':
            setEngineSpeed(-instruction.argument);
            break;

        case 'B':
            brake(instruction.argument);
            break;

        case '<':
            steer(-instruction.argument);
            break;

        case '>':
            steer(instruction.argument);
            break;

        case '^':
            steer(0);
            break;

        case 'L':
            digitalWrite(FRONT_LIGHTS_PIN, (instruction.argument == 0));
            break;

        case 'A':
            alarmLights(instruction.argument);
            break;

        case 'P':
            pursuitMode = (instruction.argument != 0);
            break;

        case 'W':
            delay(instruction.argument * 1000);
            break;
        
        default:
            return false;
    }

    return true;
}


void runScript(void* taskParams)
{
    Tracer tracer(F("runScript"));
    TRACE(F("Core ID: %d\n"), xPortGetCoreID());

    logEvent("Script started");

    terminateScript = false;
    currentInstructionIndex = 0;

    while ((currentInstructionIndex <= lastInstructionIndex) && !terminateScript)
    {
        Instruction& currentInstruction = instructions[currentInstructionIndex];
        if (!executeInstruction(currentInstruction))
        {
            String event = "Unknown command: ";
            event += currentInstruction.command;
            logEvent(event);
        }

        // Determine current loop (if any)
        LoopInfo* currentLoopPtr = nullptr;
        for (int i = 0; i <= lastLoopIndex; i++)
        {
            if (loops[i].isInLoop(currentInstructionIndex))
                currentLoopPtr = &loops[i];
        }

        if ((currentLoopPtr != nullptr) && (currentInstructionIndex == currentLoopPtr->endIndex) && (--currentLoopPtr->count > 0))
        {
            TRACE(F("Looping back to %d. Count: %d\n"), currentLoopPtr->startIndex, currentLoopPtr->count);
            currentInstructionIndex = currentLoopPtr->startIndex;
        }
        else
            currentInstructionIndex++;
    }

    currentInstructionIndex = -1;

    if (terminateScript)
        logEvent("Script terminated");
    else
        logEvent("Script completed");

    TaskHandle_t taskHandle = scriptRunnerTaskHandle;
    scriptRunnerTaskHandle = nullptr;
    vTaskDelete(taskHandle);
}


bool parseScript()
{
    Tracer tracer(F("parseScript"));

    if (script.length() >= MAX_SCRIPT_SIZE)
    {
        logEvent("Script too long");
        return false;
    }

    static char scriptBuffer[MAX_SCRIPT_SIZE];
    strcpy(scriptBuffer, script.c_str());

    lastInstructionIndex = -1;
    lastLoopIndex = -1;
    int currentLoopIndex = -1;

    const char* token = strtok(scriptBuffer, SCRIPT_TOKEN_SEPARATORS);
    while (token != nullptr)
    {
        if (strlen(token) > 0)
        {
            if (token[0] == '(')
            {
                currentLoopIndex = ++lastLoopIndex;
                if (currentLoopIndex == MAX_LOOPS)
                {
                    logEvent("Too many loops");
                    return false;
                }
                loops[currentLoopIndex].startIndex = (lastInstructionIndex + 1);
            }
            else if (token[0] == ')')
            {
                int count = 1;
                if (strlen(token) > 1)
                    count = abs(atoi(token + 1));

                loops[currentLoopIndex].endIndex = lastInstructionIndex;
                loops[currentLoopIndex].count = count;

                if (currentLoopIndex > 0)
                    currentLoopIndex--;
            }
            else
            {
                if (lastInstructionIndex++ == MAX_INSTRUCTIONS)
                {
                    logEvent("Too many instructions");
                    return false;
                }
                instructions[lastInstructionIndex].parse(token);
            }
        }

        token = strtok(nullptr, SCRIPT_TOKEN_SEPARATORS);
    }

    return true;
}


void writeHtmlHeader(String title, bool includeHomePageLink, bool includeHeading)
{
    HttpResponse.clear();
    HttpResponse.println(F("<html>"));
    
    HttpResponse.println(F("<head>"));
    HttpResponse.printf(F("<title>%s - %s</title>\r\n"), PersistentData.hostName, title.c_str());
    HttpResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
    HttpResponse.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), ICON, ICON);
    //HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
    HttpResponse.println(F("</head>"));
    
    HttpResponse.println(F("<body>"));
    if (includeHomePageLink)
        HttpResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
    if (includeHeading)
        HttpResponse.printf(F("<h1>%s</h1>\r\n"), title.c_str());
}


void writeHtmlFooter()
{
    HttpResponse.println(F("</body>"));
    HttpResponse.println(F("</html>"));
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    writeHtmlHeader(F("Home"), true, false);

    HttpResponse.println(F("<h1>RoboCar</h1>"));

    // Execute command (if specified)
    String cmd = WebServer.arg("cmd");
    if (cmd.length() > 0)
    {
        Instruction instruction;
        instruction.parse(cmd.c_str());
        if (!executeInstruction(instruction))
            HttpResponse.printf(F("<div>Unknown command: %s</div>\r\n"), cmd.c_str());
    }

    renderControlsTable();

    HttpResponse.println(F("<p class=\"script\"><a href=\"/script\">Open Script</a></p>\r\n"));

    HttpResponse.println(F("<h2>Status</h2>"));
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Engine speed</td><td>%d</td></tr>\r\n"), engineSpeed);
    HttpResponse.printf(F("<tr><td>Steer position</td><td>%d</td></tr>\r\n"), steerPosition);
    HttpResponse.printf(F("<tr><td>Pursuit mode</td><td>%d</td></tr>\r\n"), pursuitMode);
    HttpResponse.printf(F("<tr><td>Ranging Result</td><td>%d</td></tr>\r\n"), lastRangingResult);
    HttpResponse.printf(F("<tr><td>Range Status</td><td>%d</td></tr>\r\n"), lastRangingMeasurement.RangeStatus);
    HttpResponse.printf(F("<tr><td>Distance</td><td>%d mm</td></tr>\r\n"), lastRangingMeasurement.RangeMilliMeter);
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f min</td></tr>\r\n"), float(WiFiSM.getUptime()) / 60);
    HttpResponse.printf(F("<tr><td>Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h2>Event log</h2>"));
    renderEventLog();

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void renderControlsTable()
{
    static const char* controls[6][7] = {
        { "F3", "_Forward",       "B", "_Brake",      "P0",      "P1","_Pursuit" },
        { "F2",         "",        "",       "",        "",        "",        "" },
        { "F1",         "",   "_Left",       "", "_Center",        "",  "_Right" },
        { "F0",    "_Idle",   "&lt;3",  "&lt;1",       "^",   "&gt;1",   "&gt;3" },
        { "R1",         "",        "",       "",        "",        "",        "" },
        { "R2", "_Reverse",      "L0",      "L1", "_Lights",     "A5",  "_Alarm" },
        };

    HttpResponse.println(F("<table class=\"controls\">"));

    for (int row = 0; row < 6; row++)
    {
        HttpResponse.print(F("<tr>"));
        for (int col = 0; col < 7; col++)
        {
            // TODO: highlight
            const char* control = controls[row][col];
            if (strlen(control) == 0) // Empty cell
                HttpResponse.print(F("<td></td>"));
            else if (control[0] == '_') // Label
                HttpResponse.printf(F("<td class=\"label\">%s</td>"), control + 1);
            else // Control
                HttpResponse.printf(F("<td class=\"control\"><a class=\"control\" href=\"?cmd=%s\">%s</a></td>"), control, control);
        }
        HttpResponse.println("</tr>");
    }

    HttpResponse.println(F("</table>"));
}


void handleHttpScriptRequest()
{
    Tracer tracer(F("handleHttpScriptRequest"));

    writeHtmlHeader(F("Script"), true, true);

    HttpResponse.println(F("<form action=\"/script\" method=\"POST\">"));
    HttpResponse.println(F("<textarea name=\"script\" rows=\"10\" cols=\"40\">"));
    HttpResponse.println(script);
    HttpResponse.println(F("</textarea><br>"));
    HttpResponse.println(F("<input type=\"submit\" value=\"Run\">"));
    HttpResponse.println(F("</form>"));

    parseScript();

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>#Instructions</td><td>%d</td></tr>\r\n"), lastInstructionIndex + 1);
    HttpResponse.printf(F("<tr><td>#Loops</td><td>%d</td></tr>\r\n"), lastLoopIndex + 1);
    HttpResponse.printf(F("<tr><td>Instruction</td><td>%d</td></tr>\r\n"), currentInstructionIndex);
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpScriptPostRequest()
{
    Tracer tracer(F("handleHttpScriptPostRequest"));

    script = WebServer.arg("script");
    if (parseScript())
        spawnScriptRunner();

    handleHttpScriptRequest();
}


void renderEventLog()
{
    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));
}


void handleHttpEventLogClearRequest()
{
    Tracer tracer(F("handleHttpEventLogClearRequest"));

    EventLog.clear();
    logEvent(F("Event log cleared."));

    handleHttpRootRequest();
}


void addTextBoxRow(StringBuilder& output, String name, String value, String label)
{
    output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\"></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        value.c_str()
        );
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    char tzOffsetString[4];
    sprintf(tzOffsetString, "%d", PersistentData.timeZoneOffset);

    writeHtmlHeader(F("Configuration"), true, true);

    HttpResponse.println(F("<form action=\"/config\" method=\"POST\">"));
    HttpResponse.println(F("<table>"));
    addTextBoxRow(HttpResponse, F("hostName"), PersistentData.hostName, F("Host name"));
    addTextBoxRow(HttpResponse, F("tzOffset"), tzOffsetString, F("Timezone offset"));
    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    String tzOffsetString = WebServer.arg("tzOffset");

    strcpy(PersistentData.hostName, WebServer.arg("hostName").c_str()); 

    PersistentData.timeZoneOffset = static_cast<int8_t>(atoi(tzOffsetString.c_str()));

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
