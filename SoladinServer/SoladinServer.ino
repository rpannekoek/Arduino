#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <FS.h>
#include <Soladin.h>
#include <WiFiNTP.h>
#include <Tracer.h>
#include <PrintHex.h>
#include <StringBuilder.h>
#include "WiFiCredentials.private.h"

 // Use same baud rate for debug output as ROM boot code
#define DEBUG_BAUDRATE 74880
// 36 seconds poll interval => 100 polls per hour
#define POLL_INTERVAL 36
#define MIN_NIGHT_DURATION (4 * 3600)
#define MAX_EVENT_LOG_SIZE 100
#define MAX_BAR_LENGTH 50
#define ICON "/apple-touch-icon.png"
const char* DAY_LABELS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const float POLLS_PER_HOUR = 3600 / POLL_INTERVAL;

SoladinComm Soladin;
ESP8266WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer("0.europe.pool.ntp.org", 24 * 3600); // Synchronize daily
StringBuilder HtmlResponse(16384); // 16KB HTML response buffer
bool isInitialized = false;
bool soladinIsOn = false;
time_t lastPollTime = 0;
time_t startOfDayTime = 0;
time_t soladinLastOnTime = 0;
time_t maxPowerTime = 0;
struct tm startOfDay;
float lastGridEnergy = 0;
int currentHour = 0;
int currentDay = 0;
int currentWeek = 0;
int maxPower = 0;
float energyPerHour[24];
float energyPerDay[7];
float energyPerWeek[52];
uint8_t weekDays[7];
String weekLabels[52];
String eventLog[MAX_EVENT_LOG_SIZE];
int eventLogEnd = 0;


struct PersistentDataClass
{
  public:
    word Magic = 0x55AA55AA;
    char HostName[16];
    int TimeZoneOffset;

    // Constructor
    PersistentDataClass()
    {
      EEPROM.begin(512);
    }

    // Destructor
    ~PersistentDataClass()
    {
      EEPROM.end();
    }

    void writeToEEPROM()
    {
      Tracer tracer("PersistentDataClass::writeToEEPROM");

      byte* bytePtr = (byte*) this;
      Serial.printf("Writing %d bytes to EEPROM from %p ...\n", _dataSize, bytePtr);
      printHex(bytePtr, _dataSize);

      Magic = _initialized_magic;
      for (size_t i = 0; i < _dataSize; i++)
        EEPROM.write(i, *bytePtr++);

      EEPROM.commit();
    }

    bool readFromEEPROM()
    {
      Tracer tracer("PersistentDataClass::readFromEEPROM");

      byte* bytePtr = (byte*) this;
      Serial.printf("Reading %d bytes from EEPROM to %p ...\n", _dataSize, bytePtr);      

      for (size_t i = 0; i < _dataSize; i++)
        *bytePtr++ = EEPROM.read(i);

      printHex((byte*) this, _dataSize);

      return Magic == _initialized_magic;
    }

  private:
    const size_t _dataSize = sizeof(Magic) + sizeof(HostName) + sizeof(TimeZoneOffset);
    const word _initialized_magic = 0xCAFEBABE;
};

PersistentDataClass PersistentData;


bool initializeDay()
{
  Tracer tracer("initializeDay");
  
  currentHour = 0;
  maxPower = 0;
  maxPowerTime = 0;
  memset(energyPerHour, 0, sizeof(energyPerHour));

  startOfDayTime = TimeServer.getCurrentTime();
  if (startOfDayTime < 1000) return false;

  struct tm* gmtTimePtr = gmtime(&startOfDayTime);
  memcpy(&startOfDay, gmtTimePtr, sizeof(startOfDay)); 
  Serial.print(asctime(&startOfDay));  

  weekDays[currentDay] = startOfDay.tm_wday;

  return true;
}


void initializeWeek()
{
  Tracer tracer("initializeWeek");

  memset(energyPerDay, 0, sizeof(energyPerDay));

  char weekLabel[16];
  strftime(weekLabel, sizeof(weekLabel), "%e %h", &startOfDay);
  weekLabels[currentWeek] = weekLabel;
}


void initializeYear()
{
  Tracer tracer("initializeYear");

  memset(energyPerWeek, 0, sizeof(energyPerWeek));
}


int formatTime(char* output, size_t output_size, const char* format, time_t time)
{
  time += PersistentData.TimeZoneOffset * 3600;
  return strftime(output, output_size, format, gmtime(&time));
}


void logEvent(String msg)
{
  Tracer tracer("logEvent", msg.c_str());
    
  char timestamp[32];
  formatTime(timestamp, sizeof(timestamp), "%F %H:%M:%S : ", TimeServer.getCurrentTime());

  String event = timestamp;
  event += msg;
  eventLog[eventLogEnd++] = event;

  if (eventLogEnd == MAX_EVENT_LOG_SIZE)
  {
    Serial.println("Event log is full");
    for (int i = 1; i < eventLogEnd; i++) eventLog[i - 1] = eventLog[i];
    eventLogEnd--;
  }
}


// Boot code
void setup() 
{
  Serial.begin(DEBUG_BAUDRATE);
  Serial.println();

  // Turn built-in LED on
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);

  // Read persistent data from EEPROM or initialize to defaults.
  if (!PersistentData.readFromEEPROM())
  {
    Serial.println("EEPROM not initialized; initializing with defaults.");
    strcpy(PersistentData.HostName, "Soladin");
    PersistentData.TimeZoneOffset = 0;
  }
  
  // Connect to WiFi network
  Serial.printf("Connecting to WiFi network '%s' ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(PersistentData.HostName);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int i = 0;
  while (WiFi.status() == WL_IDLE_STATUS || WiFi.status() == WL_DISCONNECTED)
  {
    Serial.print(".");
    delay(500);
    if (i++ == 60)
    {
      Serial.printf("\nTimeout connecting WiFi.");
      return;
    }
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.printf("\nError connecting WiFi. Status: %d\n", WiFi.status());
    return;
  }
  Serial.print("\nWiFi connected. IP address: ");
  Serial.println(WiFi.localIP());

  if (!initializeDay())
  {
    Serial.println("Time Server sync failed.");
    return;
  }
  soladinLastOnTime = startOfDayTime; // Prevent immediate day rollover

  initializeWeek();
  initializeYear();

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
  WebServer.begin();
  Serial.println("Web Server started");
  
  // Turn built-in LED off
  digitalWrite(LED_BUILTIN, 1);

  logEvent(F("Initialized after boot."));
  isInitialized = true;
}


// Called repeatedly
void loop() 
{
  if (!isInitialized)
  {
    // Initialization failed. Blink LED.
    digitalWrite(LED_BUILTIN, 0);
    delay(500);
    digitalWrite(LED_BUILTIN, 1);
    delay(500);
    return;
  }
  
  WebServer.handleClient();

  if (Serial.available())
  {
    digitalWrite(LED_BUILTIN, 0);
    handleSerialRequest();
    digitalWrite(LED_BUILTIN, 1);
  }

  time_t currentTime = TimeServer.getCurrentTime();
  if (currentTime >= lastPollTime + POLL_INTERVAL)
  {
    digitalWrite(LED_BUILTIN, 0);
    pollSoladin(currentTime);
    digitalWrite(LED_BUILTIN, 1);
    lastPollTime = currentTime;
  }

  delay(10);
}


void pollSoladin(time_t currentTime)
{
  char currentTimeString[16];
  sprintf(currentTimeString, "%u", (unsigned int) currentTime);
  Tracer tracer("pollSoladin", currentTimeString);

  soladinIsOn = Soladin.getDeviceStats();
  if (!soladinIsOn)
  {
    Serial.println("Soladin is off.");
    return;
  }
  
  if (currentTime > (soladinLastOnTime + MIN_NIGHT_DURATION))
  {
    Serial.println("Start of new day");
    currentDay = (currentDay + 1) % 7;
    initializeDay();
    if (currentDay == 0)
    {
      Serial.println("Start of new week.");
      currentWeek = (currentWeek + 1) % 52;
      initializeWeek();
      if (currentWeek == 0)
      {
        Serial.println("Start of new year.");
        initializeYear();
      }
    }
  }

  soladinLastOnTime = currentTime;
  currentHour = (currentTime - startOfDayTime) / 3600;

  if (Soladin.GridPower > maxPower)
  {
    maxPower = Soladin.GridPower;
    maxPowerTime = currentTime;
  }

  float gridEnergyDelta = (lastGridEnergy == 0)  ? 0 : (Soladin.GridEnergy - lastGridEnergy); // kWh
  lastGridEnergy = Soladin.GridEnergy;
  Serial.printf("gridEnergyDelta = %f kWh; lastGridEnergy = %f kWh\n", gridEnergyDelta, lastGridEnergy);
  
  energyPerHour[currentHour] += static_cast<float>(Soladin.GridPower) / POLLS_PER_HOUR; // This has higher resolution than gridEnergyDelta
  energyPerDay[currentDay] += gridEnergyDelta;
  energyPerWeek[currentWeek] += gridEnergyDelta; 

  Serial.printf("energyPerHour[%d] = %f\n", currentHour, energyPerHour[currentHour]);
  Serial.printf("energyPerDay[%d] = %f\n", currentDay, energyPerDay[currentDay]);
  Serial.printf("energyPerWeek[%d] = %f\n", currentWeek, energyPerWeek[currentWeek]);

  if (Soladin.Flags.length() > 0)
    logEvent(Soladin.Flags);
}


void webTest()
{
    // Setup some test data
    for (int i = 0; i < 24; i++) energyPerHour[i] = i * 20;
    for (int i = 0; i < 7; i++) 
    {
      energyPerDay[i] = i / 2.0;
      weekDays[i] = i;
    }
    for (int i = 0; i < 52; i++) 
    {
      energyPerWeek[i] = i / 2.0;
      weekLabels[i] = "Test";
    }

    currentHour = 23;
    currentDay = 6;
    currentWeek = 51;
    
    // Simulate multiple incoming root requests
    for (int i = 0; i < 100; i++) handleHttpRootRequest();
}


void handleSerialRequest()
{
  Tracer tracer("handleSerialRequest");
  Serial.setTimeout(10);

  char cmd;
  if (!Serial.readBytes(&cmd, 1)) return;
  Serial.println(cmd);

  if (cmd == 't')
    for (int i = 0; i < 100; i++) Soladin.probe();
  else if (cmd == 'p')
    Soladin.probe();
  else if (cmd == 's')
    Soladin.getDeviceStats();
  else if (cmd == 'w')
    webTest();
  else if (cmd == 'f')
  {
    long freeHeap = ESP.getFreeHeap();
    Serial.printf("Free heap: %u\n", static_cast<word>(freeHeap));
  }
}


void addRow(StringBuilder& output, const char* label, float value, const char* unitOfMeasure, const char* valueFormat = NULL)
{
  if (valueFormat == NULL)
    valueFormat = "0.0";
  
  char format[64];
  sprintf(format, "<tr><td>%%s</td><td>%%%sf %%s</td></tr>\r\n", valueFormat);

  output.printf(format, label, value, unitOfMeasure);
}


void addGraphRow(StringBuilder& output, const char* label, float value, float maxValue, const char* unitOfMeasure)
{
  float scale = (value / maxValue) * MAX_BAR_LENGTH;
  int barLength = static_cast<int>(scale + 0.5);
  if (barLength > MAX_BAR_LENGTH)
    barLength = MAX_BAR_LENGTH;

  char bar[MAX_BAR_LENGTH + 1];
  memset(bar, 'o', barLength);
  bar[barLength] = 0;  

  output.printf(F("<tr><td>%s</td><td>%0.2f %s</td><td><span class=\"bar\">%s</span></td></tr>\r\n"), label, value, unitOfMeasure, bar);
}


void handleHttpRootRequest()
{
  Tracer tracer("handleHttpRootRequest");
  
  float pvPower = Soladin.PvVoltage * Soladin.PvCurrent;

  String status;
  if (soladinIsOn)
  {
    if (Soladin.Flags.length() == 0)
      status = "On";
    else
      status = Soladin.Flags;
  }
  else
    status = "Off";

  HtmlResponse.clear();
  HtmlResponse.println(F("<html>"));
  HtmlResponse.println(F("<head>"));
  HtmlResponse.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , POLL_INTERVAL);
  HtmlResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
  HtmlResponse.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), ICON, ICON);
  HtmlResponse.printf(F("<title>%s</title>\r\n</head>\r\n"), PersistentData.HostName);
  HtmlResponse.println(F("<body>"));

  HtmlResponse.println(F("<h1>Soladin device stats</h1>"));
  HtmlResponse.println(F("<table class=\"devstats\">"));
  HtmlResponse.printf(F("<tr><td>Status</td><td>%s</td></tr>\r\n"), status.c_str());
  addRow(HtmlResponse, "PV Voltage", Soladin.PvVoltage, "V", "0.1");
  addRow(HtmlResponse, "PV Current", Soladin.PvCurrent, "A", "0.2");
  addRow(HtmlResponse, "PV Power", pvPower, "W", "0.1");
  addRow(HtmlResponse, "Grid Voltage", Soladin.GridVoltage, "V");
  addRow(HtmlResponse, "Grid Frequency", Soladin.GridFrequency, "Hz", "0.2");
  addRow(HtmlResponse, "Grid Power", Soladin.GridPower, "W");
  addRow(HtmlResponse, "Grid Energy", Soladin.GridEnergy, "kWh", "0.2");
  addRow(HtmlResponse, "Temperature", Soladin.Temperature, "C");
  if (pvPower > 0)
  {
    addRow(HtmlResponse, "Efficiency", Soladin.GridPower / pvPower * 100, "%");
  }
  addRow(HtmlResponse, "Max Grid Power", maxPower, "W");
  if (maxPowerTime > 0)
  {
    char timestamp[16];
    formatTime(timestamp, sizeof(timestamp), "%H:%M", maxPowerTime);
    HtmlResponse.printf(F("<tr><td>Max Power Time</td><td>%s</td></tr>\r\n"), timestamp);
  }
  HtmlResponse.println(F("</table>"));
  
  HtmlResponse.printf(F("<p class=\"events\"><a href=\"/events\">%d events logged.</a></p>\r\n"), eventLogEnd);

  HtmlResponse.println(F("<h1>Energy per hour</h1>"));
  HtmlResponse.println(F("<table class=\"nrg\">"));
  char label[8];
  for (int hour = 0; hour <= currentHour; hour++)
  {
    sprintf(label, "%02d:%02d", startOfDay.tm_hour + hour + PersistentData.TimeZoneOffset, startOfDay.tm_min);
    addGraphRow(HtmlResponse, label, energyPerHour[hour], 500, "Wh");
  }
  HtmlResponse.println(F("</table>"));

  HtmlResponse.println(F("<h1>Energy per day</h1>"));
  HtmlResponse.println(F("<table class=\"nrg\">"));
  for (int day = 0; day <= currentDay; day++)
  {
    addGraphRow(HtmlResponse, DAY_LABELS[weekDays[day]], energyPerDay[day], 5, "kWh");
  }
  HtmlResponse.println(F("</table>"));

  HtmlResponse.println(F("<h1>Energy per week</h1>"));
  HtmlResponse.println(F("<table class=\"nrg\">"));
  for (int week = 0; week <= currentWeek; week++)
  {
    addGraphRow(HtmlResponse, weekLabels[week].c_str(), energyPerWeek[week], 35, "kWh");
  }
  HtmlResponse.println(F("</table>"));

  HtmlResponse.println(F("</body></html>"));

  WebServer.send(200, "text/html", HtmlResponse);
}


void handleHttpEventLogRequest()
{
  Tracer tracer("handleHttpEventLogRequest");

  HtmlResponse.clear();
  HtmlResponse.println(F("<html>"));
  HtmlResponse.println(F("<head>"));
  HtmlResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
  HtmlResponse.println(F("</head>"));
  HtmlResponse.println(F("<body>"));
  HtmlResponse.println(F("<a href=\"/\"><img src=\"" ICON "\"></a>"));
  HtmlResponse.println(F("<h1>Event log</h1>"));

  for (int i = 0; i < eventLogEnd; i ++)
  {
    HtmlResponse.printf(F("<div>%s</div>\r\n"), eventLog[i].c_str());
  }

  HtmlResponse.println(F("<div><a href=\"/events/clear\">Clear event log</a></div>"));
  HtmlResponse.println(F("</body></html>"));

  WebServer.send(200, "text/html", HtmlResponse);
}


void handleHttpEventLogClearRequest()
{
  Tracer tracer("handleHttpEventLogClearRequest");
  eventLogEnd = 0;
  logEvent("Event log cleared.");
  handleHttpEventLogRequest();
}


void addTextBoxRow(StringBuilder& output, const char* name, const char* value, const char* label)
{
  output.printf(F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\"></td></tr>\r\n"), name, label, name, value);
}


void handleHttpConfigFormRequest()
{
  Tracer tracer("handleHttpConfigFormRequest");

  char tzOffsetString[4];
  sprintf(tzOffsetString, "%d", PersistentData.TimeZoneOffset);

  // TODO: link to home page
  HtmlResponse.clear();
  HtmlResponse.println(F("<html>"));
  HtmlResponse.println(F("<head>"));
  HtmlResponse.println(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"/styles.css\">"));
  HtmlResponse.println(F("</head>"));
  HtmlResponse.println(F("<body>"));
  HtmlResponse.println(F("<h1>Configuration</h1>"));
  HtmlResponse.println(F("<form action=\"/config\" method=\"POST\">"));
  HtmlResponse.println(F("<table>"));
  addTextBoxRow(HtmlResponse, "hostName", PersistentData.HostName, "Host name");
  addTextBoxRow(HtmlResponse, "tzOffset", tzOffsetString, "Timezone offset");
  HtmlResponse.println(F("</table>"));
  HtmlResponse.println(F("<input type=\"submit\">"));
  HtmlResponse.println(F("</form>"));
  HtmlResponse.println(F("</body></html>"));

  WebServer.send(200, "text/html", HtmlResponse);
}


void handleHttpConfigFormPost()
{
  Tracer tracer("handleHttpConfigFormPost");

  strcpy(PersistentData.HostName, WebServer.arg("hostName").c_str()); 
  String tzOffsetString = WebServer.arg("tzOffset");

  Serial.printf("hostName: %s\n", PersistentData.HostName);
  Serial.printf("tzOffset: %s\n", tzOffsetString.c_str());
  
  sscanf(tzOffsetString.c_str(), "%d", &PersistentData.TimeZoneOffset);

  PersistentData.writeToEEPROM();

  handleHttpConfigFormRequest();

  // TODO: restart (?)
}


void handleHttpNotFound()
{
  Serial.printf("Unexpected HTTP request: %s\n", WebServer.uri().c_str());
  WebServer.send(404, "text/plain", "Unexpected request.");
}
