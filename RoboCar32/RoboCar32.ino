#include <math.h>
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
#define POLL_INTERVAL 30
#define MIN_RANGE_MM 300

#define SCRIPT_TOKEN_SEPARATORS " ,\r\n"
#define MAX_SCRIPT_SIZE 1204
#define MAX_INSTRUCTIONS 256
#define MAX_LOOPS 8

#define STEER_LEFT_PIN 16
#define STEER_RIGHT_PIN 17
#define ENGINE_FWD_PIN 18
#define ENGINE_REV_PIN 19
#define FRONT_LIGHTS_PIN 4
#define BRAKE_LIGHTS_PIN 15
#define INDICATOR_LEFT_PIN 0
#define INDICATOR_RIGHT_PIN 2

#define ENGINE_PWM_CHANNEL 0
#define INDICATOR_PWM_CHANNEL 1

#define ENGINE_PWM_FREQ 50
#define INDICATOR_PWM_FREQ 2


struct Instruction
{
    char command;
    int8_t argument;

    void parse(const char* token)
    {
        Tracer tracer(F("Instruction::parse"), token);

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

VL53L0X_Error lastRangingResult;
VL53L0X_RangingMeasurementData_t lastRangingMeasurement;

time_t currentTime;

int8_t engineSpeed = 0;
int8_t steerPosition = 0;

String script = F("L ( F W B R W B < W > W ^ W )2 L0");
Instruction instructions[MAX_INSTRUCTIONS];
LoopInfo loops[MAX_LOOPS];
int currentInstructionIndex = -1;
int lastInstructionIndex = -1;
int lastLoopIndex = -1;
bool terminateScript;

TaskHandle_t rangeMonitorTaskHandle;
TaskHandle_t scriptRunnerTaskHandle;


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
    WebServer.on("/", HTTP_POST, handleHttScriptPostRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico", cacheControl);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic("/styles.css", SPIFFS, "/styles.css", cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.begin(WIFI_SSID, WIFI_PASSWORD, PersistentData.hostName);

    if (!DistanceSensor.begin(VL53L0X_I2C_ADDR, true))
        TRACE(F("Cannot initialize distance sensor\n"));

    initEngineDrive();
    initSteering();
    initLights();

    spawnRangeMonitor();

    parseScript();

    Tracer::traceFreeHeap();

    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);
}


bool spawnRangeMonitor()
{
    Tracer tracer(F("spawnRangeMonitor"));

    xTaskCreatePinnedToCore(
        monitorForwardRange,
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

    delay(10);
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");

    static char cmd[32];
    size_t bytesRead = Serial.readBytesUntil('\n', cmd, sizeof(cmd));
    cmd[bytesRead] = 0;

    Instruction instruction;
    instruction.parse(cmd);
    if (!executeInstruction(instruction))
    {
        TRACE(F("Unexpected command: %s\n"), cmd);
    }
}


void initLights()
{
    Tracer(F("initLights"));

    pinMode(BRAKE_LIGHTS_PIN, OUTPUT);

    // Turn all lights on
    digitalWrite(FRONT_LIGHTS_PIN, LOW);
    digitalWrite(BRAKE_LIGHTS_PIN, LOW);
    digitalWrite(INDICATOR_LEFT_PIN, LOW);
    digitalWrite(INDICATOR_RIGHT_PIN, LOW);

    delay(500);

    // Turn all lights off
    digitalWrite(FRONT_LIGHTS_PIN, HIGH);
    digitalWrite(BRAKE_LIGHTS_PIN, HIGH);
    digitalWrite(INDICATOR_LEFT_PIN, HIGH);
    digitalWrite(INDICATOR_RIGHT_PIN, HIGH);

    // Initialize indicator PWM
    ledcSetup(INDICATOR_PWM_CHANNEL, INDICATOR_PWM_FREQ, 8);
    ledcWrite(INDICATOR_PWM_CHANNEL, 128);
}


void initEngineDrive()
{
    Tracer(F("initEngineDrive"));

    pinMode(ENGINE_FWD_PIN, OUTPUT);
    pinMode(ENGINE_REV_PIN, OUTPUT);

    digitalWrite(ENGINE_FWD_PIN, LOW);
    digitalWrite(ENGINE_REV_PIN, LOW);

    ledcSetup(ENGINE_PWM_CHANNEL, ENGINE_PWM_FREQ, 8);
}


void initSteering()
{
    Tracer(F("initSteering"));

    pinMode(STEER_LEFT_PIN, OUTPUT);
    pinMode(STEER_RIGHT_PIN, OUTPUT);

    digitalWrite(STEER_LEFT_PIN, LOW);
    digitalWrite(STEER_RIGHT_PIN, LOW);

    // TODO: center
}


void setEngineSpeed(int8_t speed)
{
    Tracer tracer(F("setEngineSpeed"));

    uint32_t pwmDuty = min(abs(speed) << 5, 255);
    TRACE(F("Engine PWM duty: %u\n"), pwmDuty);
    ledcWrite(ENGINE_PWM_CHANNEL, pwmDuty);

    if (speed >= 0)
    {
        digitalWrite(ENGINE_REV_PIN, LOW);
        ledcAttachPin(ENGINE_FWD_PIN, ENGINE_PWM_CHANNEL);
    }
    else
    {
        digitalWrite(ENGINE_FWD_PIN, LOW);
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

    delay(seconds * 1000);

    digitalWrite(ENGINE_FWD_PIN, LOW);
    digitalWrite(ENGINE_REV_PIN, LOW);
    digitalWrite(BRAKE_LIGHTS_PIN, HIGH);

    engineSpeed = 0;
}


void steer(int8_t toPosition)
{
    Tracer tracer(F("steer"));

    if (toPosition == 0)
    {
        // Turn indicator lights off
        if (steerPosition > 0)
            ledcDetachPin(INDICATOR_RIGHT_PIN);
        else if (steerPosition < 0)
            ledcDetachPin(INDICATOR_LEFT_PIN);
    }
    else
    {
        // Turn indicator lights on
        if (toPosition > 0)
            ledcAttachPin(INDICATOR_RIGHT_PIN, INDICATOR_PWM_CHANNEL);
        else
            ledcAttachPin(INDICATOR_LEFT_PIN, INDICATOR_PWM_CHANNEL);
    }

    if (toPosition == steerPosition)
        return;

    if (toPosition > steerPosition)
    {
        digitalWrite(STEER_RIGHT_PIN, HIGH);
        digitalWrite(STEER_LEFT_PIN, LOW);
    }
    else
    {
        digitalWrite(STEER_LEFT_PIN, HIGH);
        digitalWrite(STEER_RIGHT_PIN, LOW);
    }

    // TODO: await feedback from rotary encoder
    delay(500);

    digitalWrite(STEER_LEFT_PIN, LOW);
    digitalWrite(STEER_RIGHT_PIN, LOW);

    steerPosition = toPosition;
}


void monitorForwardRange(void* taskParams)
{
    Tracer tracer(F("monitorForwardRange"));
    TRACE(F("Core ID: %d\n"), xPortGetCoreID());

    char event[32];

    while (true)
    {
        delay(10);

        if (engineSpeed <= 0)
            continue;

        lastRangingResult = DistanceSensor.rangingTest(&lastRangingMeasurement);
        if (lastRangingResult != VL53L0X_ERROR_NONE)
        {
            snprintf(event, sizeof(event), "Ranging error: %d", lastRangingResult);
            logEvent(event);
            terminateScript = true; 
            brake(1);
            continue;
        }

        if (lastRangingMeasurement.RangeStatus == 4) // Out of range
            continue;

        if (lastRangingMeasurement.RangeMilliMeter < MIN_RANGE_MM)
        {
            snprintf(event, sizeof(event), "Collision detection: %d mm", lastRangingMeasurement.RangeMilliMeter);
            logEvent(event);
            terminateScript = true; 
            brake(2);
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
            digitalWrite(FRONT_LIGHTS_PIN, instruction.argument);
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

        if ((currentLoopPtr != nullptr) && (currentInstructionIndex == currentLoopPtr->endIndex) && (currentLoopPtr->count-- > 0))
            currentInstructionIndex = currentLoopPtr->startIndex;
        else
            currentInstructionIndex++;
    }

    currentInstructionIndex = -1;

    if (terminateScript)
        logEvent("Script terminated");
    else
        logEvent("Script completed");

    vTaskDelete(scriptRunnerTaskHandle);
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
    HttpResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
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
    
    writeHtmlHeader(F("Home"), false, false);

    HttpResponse.println(F("<h1>RoboCar</h1>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f min</td></tr>\r\n"), float(WiFiSM.getUptime()) / 60);
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<h2>Ranging</h2>\r\n"), lastRangingMeasurement.RangeMilliMeter);
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Result</td><td>%d</td></tr>\r\n"), lastRangingResult);
    HttpResponse.printf(F("<tr><td>Status</td><td>%d</td></tr>\r\n"), lastRangingMeasurement.RangeStatus);
    HttpResponse.printf(F("<tr><td>Distance</td><td>%d mm</td></tr>\r\n"), lastRangingMeasurement.RangeMilliMeter);
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), EventLog.count());

    HttpResponse.println(F("<h2>Script</h2>"));
    HttpResponse.println(F("<form action=\"/\" method=\"POST\">"));
    HttpResponse.println(F("<textarea name=\"script\" rows=\"10\" cols=\"40\">"));
    HttpResponse.println(script);
    HttpResponse.println(F("</textarea><br>"));
    HttpResponse.println(F("<input type=\"submit\" value=\"Run\">"));
    HttpResponse.println(F("</form>"));

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>#Instructions</td><td>%d</td></tr>\r\n"), lastInstructionIndex + 1);
    HttpResponse.printf(F("<tr><td>#Loops</td><td>%d</td></tr>\r\n"), lastLoopIndex + 1);
    HttpResponse.printf(F("<tr><td>Instruction</td><td>%d</td></tr>\r\n"), currentInstructionIndex);
    HttpResponse.println(F("</table>"));

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttScriptPostRequest()
{
    Tracer tracer(F("handleHttScriptPostRequest"));

    script = WebServer.arg("script");
    if (parseScript())
        spawnScriptRunner();

    handleHttpRootRequest();
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    writeHtmlHeader(F("Event log"), true, true);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));

    writeHtmlFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpEventLogClearRequest()
{
    Tracer tracer(F("handleHttpEventLogClearRequest"));

    EventLog.clear();
    logEvent(F("Event log cleared."));

    handleHttpEventLogRequest();
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
