#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#define STDINT_H
#include <Adafruit_VL53L0X.h>
#include "PersistentData.h"
#include "WiFiCredentials.private.h"

#define DEBUG_ESP_PORT Serial
#define MAX_EVENT_LOG_SIZE 100
#define ICON "/apple-touch-icon.png"
#define NTP_SERVER "fritz.box"
#define POLL_INTERVAL 30

WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(NTP_SERVER, 24 * 3600); // Synchronize daily
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
Adafruit_VL53L0X DistanceSensor = Adafruit_VL53L0X();

VL53L0X_Error lastRangingResult;
VL53L0X_RangingMeasurementData_t lastRangingMeasurement;

time_t currentTime;
time_t rangingTime = 0;

int pwmPin = 23;
int pwmFrequency = 2;
uint8_t pwmDuty = 128;


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

    pinMode(pwmPin, OUTPUT);
    digitalWrite(pwmPin, HIGH);

    Serial.begin(115200);
    Serial.setTimeout(1000);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    
    SPIFFS.begin();

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
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

    Tracer::traceFreeHeap();
    
    // Turn built-in LED off
    digitalWrite(LED_BUILTIN, 1);
}


// Called repeatedly
void loop() 
{
    uint32_t currentMillis = millis();

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
    TRACE(F("%s\n"), cmd);

    if (cmd[0] == 'f')
    {
        pwmFrequency = atoi(cmd + 1);
        configurePWM();
    }
    else if (cmd[0] == 'd')
    {
        pwmDuty = atoi(cmd + 1);
        configurePWM();
    }
    else if (cmd[0] == 'r')
        rangingTest();
}


void configurePWM()
{
    TRACE(F("Freq: %d; Duty: %d\n"), pwmFrequency, pwmDuty);
    ledcSetup(0, pwmFrequency, 8);
    ledcAttachPin(pwmPin, 0);
    ledcWrite(0, pwmDuty);
}


void rangingTest()
{
    Tracer tracer(F("rangingTest"));

    lastRangingResult = DistanceSensor.rangingTest(&lastRangingMeasurement, true);
    TRACE(F("lastRangingResult result: %d\n"), lastRangingResult);
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
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    HttpResponse.println(F("</table>"));

    rangingTest();

    HttpResponse.printf(F("<h2>Ranging</h2>\r\n"), lastRangingMeasurement.RangeMilliMeter);
    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><td>Result</td><td>%d</td></tr>\r\n"), lastRangingResult);
    HttpResponse.printf(F("<tr><td>Status</td><td>%d</td></tr>\r\n"), lastRangingMeasurement.RangeStatus);
    HttpResponse.printf(F("<tr><td>Distance</td><td>%d mm</td></tr>\r\n"), lastRangingMeasurement.RangeMilliMeter);
    HttpResponse.println(F("</table>"));

    HttpResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), EventLog.count());

    writeHtmlFooter();

    WebServer.send(200, "text/html", HttpResponse);
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
