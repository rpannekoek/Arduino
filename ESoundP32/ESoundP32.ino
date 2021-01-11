#define CONFIG_DSP_OPTIMIZED true
#define DEBUG_ESP_PORT Serial
#define CORE_DEBUG_LEVEL ARDUHAL_LOG_LEVEL_INFO

#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include "PersistentData.h"
#include "DSP32.h"

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define SAMPLE_FREQUENCY 44100
#define FRAME_SIZE 1024
#define REFRESH_INTERVAL 30
#define FTP_RETRY_INTERVAL 3600

#define LED_ON 0
#define LED_OFF 1

#define CFG_WIFI_SSID F("WifiSSID")
#define CFG_WIFI_KEY F("WifiKey")
#define CFG_HOST_NAME F("HostName")
#define CFG_NTP_SERVER F("NTPServer")
#define CFG_FTP_SERVER F("FTPServer")
#define CFG_FTP_USER F("FTPUser")
#define CFG_FTP_PASSWORD F("FTPPassword")
#define CFG_TZ_OFFSET F("TZOffset")

WebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer(3600 * 24); // Synchronize daily
WiFiFTPClient FTPClient(2000); // 2 sec timeout
StringBuilder HttpResponse(16384); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, ICON, CSS, 60); // Max bar length: 60
Log<const char> EventLog(50); // Max 50 log entries
WiFiStateMachine WiFiSM(TimeServer, WebServer, EventLog);
DSP32 DSP(true); // Trace DSP performance

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;
bool isFTPEnabled = false;

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
    // Turn built-in LED on during boot
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    Serial.begin(115200);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
    TimeServer.timeZoneOffset = PersistentData.timeZoneOffset;
    Html.setTitlePrefix(PersistentData.hostName);
    isFTPEnabled = PersistentData.ftpServer[0] != 0;

    START_SPIFFS;

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/test", handleHttpTestDSPRequest);
    WebServer.on("/sync", handleHttpSyncFTPRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/events/clear", handleHttpEventLogClearRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    //DSP.begin(FRAME_SIZE, WindowType::None, SAMPLE_FREQUENCY);

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();
    WiFiSM.run();
    delay(10);
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();
}


void onWiFiInitialized()
{
    if (Serial.available())
    {
        // TODO
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

    if (!isFTPEnabled)
    {
        logEvent(F("No FTP server configured.\n"));
        return false;
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

    if (!FTPClient.begin(PersistentData.ftpServer, PersistentData.ftpUser, PersistentData.ftpPassword, FTP_DEFAULT_CONTROL_PORT, printTo))
    {
        FTPClient.end();
        return false;
    }

    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            TRACE(F("FTP Append command failed: %s\n"), FTPClient.getLastResponse());
    }

    FTPClient.end();

    return success;
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));
    
    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    Html.writeHeader(F("Home"), false, false, REFRESH_INTERVAL);

    HttpResponse.println(F("<h1Device Status</h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>Free Heap</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>Uptime</td><td>%0.1f days</td></tr>\r\n"), float(WiFiSM.getUptime()) / 86400);
    if (isFTPEnabled)
    {
        HttpResponse.printf(F("<tr><td>FTP Sync Time</td><td>%s</td></tr>\r\n"), formatTime("%H:%M", lastFTPSyncTime));
    }
    else
    {
        HttpResponse.println(F("<tr><td>FTP Sync</td><td>Disabled</td></tr>"));
    }
    
    HttpResponse.printf(F("<tr><td><a href=\"/events\">Events logged</a></td><td>%d</td></tr>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p><a href=\"/test\">Test DSP</a></p>"));

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpTestDSPRequest()
{
    Tracer tracer(F("handleHttpTestDSPRequest"));

    String frameSizeString = WebServer.arg("frameSize");
    String windowTypeString = WebServer.arg("windowType");
    String sampleFrequencyString = WebServer.arg("fs");

    int frameSize = FRAME_SIZE;
    WindowType windowType = WindowType::None;
    int sampleFrequency = 44100;

    if (frameSizeString.length() > 0)
        frameSize = frameSizeString.toInt();
    if (windowTypeString.length() > 0)
        windowType = static_cast<WindowType>(windowTypeString.toInt());
    if (sampleFrequencyString.length() > 0)
        sampleFrequency = sampleFrequencyString.toInt();

    Html.writeHeader(F("DSP Test"), true, true);

    HttpResponse.println(F("<table>"));
    HttpResponse.printf(F("<tr><th>Frame size</th><td>%i</td></tr>\r\n"), frameSize);
    HttpResponse.printf(F("<tr><th>Window type</th><td>%i</td></tr>\r\n"), windowType);
    HttpResponse.printf(F("<tr><th>Sample frequency</th><td>%i</td></tr>\r\n"), sampleFrequency);
    HttpResponse.println(F("</table>"));

    if (DSP.begin(frameSize, windowType, sampleFrequency))
    {
        // Generate square wave signal @ Fs/64 with small DC offset
        static int16_t signal[FRAME_SIZE];
        for (int i = 0; i < FRAME_SIZE; i++)
        {
            signal[i] = ((i % 64) < 32) ? 32000 : -32000;
            signal[i] += 700; 
        }

        complex_t* complexSpectrum = DSP.runFFT(signal);
        float* spectralPower = DSP.getSpectralPower(complexSpectrum);
        float* octavePower = DSP.getOctavePower(spectralPower);
        BinInfo fundamental = DSP.getFundamental(spectralPower);

        // Output fundamental info
        HttpResponse.println(F("<h2>Fundamental analysis</h2>"));
        HttpResponse.printf(
            F("<p>bin #%i, min = %0.0f Hz, max = %0.0f Hz, center = %0.0f Hz</p>\r\n"),
            fundamental.index,
            fundamental.minFrequency,
            fundamental.maxFrequency,
            fundamental.getCenterFrequency()
            );

        // Output octave bins
        HttpResponse.println(F("<h2>Octaves<h2>"));
        HttpResponse.println(F("<table>"));
        HttpResponse.println(F("<tr><th>#</th><th>Range (Hz)</th><th>Center (Hz)</th><th>Power</th><th>dB</th></tr>"));
        for (int i = 0; i < DSP.getOctaves(); i++)
        {
            BinInfo octaveBin = DSP.getOctaveInfo(i);
            float dB = 10 * log10f(octavePower[i]); 
            HttpResponse.printf(
                F("<tr><th>%i</th><td>%0.0f-%0.0f</td><td>%0.0f</td><td>%g</td><td>%0.1f dB</td><td class=\"graph\">"),
                i + 1,
                octaveBin.minFrequency,
                octaveBin.maxFrequency,
                octaveBin.getCenterFrequency(),
                octavePower[i],
                dB
                );

            float bar = (dB + 20) / 20;
            if (bar < 0) bar = 0;
            Html.writeBar(bar, F("deliveredBar"), true);

            HttpResponse.println(F("</td></tr>"));
        }
        HttpResponse.println(F("</table>"));

        // Output (complex) spectrum and octaves
        HttpResponse.println(F("<h2>Complex spectrum</h2>"));
        HttpResponse.println(F("<table>"));
        HttpResponse.println(F("<tr><th>Bin</th><th>Complex</th><th>Magnitude</th><th>Phase</th><th>Power</th><th>Octave</th></tr>\r\n"));
        int octave = 0;
        int octaveLength = 1;
        int nextOctaveIndex = 1;
        for (int i = 0; i < 100; i++)
        {
            if (i == nextOctaveIndex)
            {
                octave++;
                nextOctaveIndex += octaveLength;
                octaveLength *= 2;
            }

            HttpResponse.printf(
                F("<tr><td>%i</td><td>%g + %g i</td><td>%g</td><td>%0.0f deg</td><td>%g</td><td>%i</td></tr>\r\n"),
                i,
                complexSpectrum[i].re,
                complexSpectrum[i].im,
                complexSpectrum[i].getMagnitude(),
                complexSpectrum[i].getPhase(),
                spectralPower[i],
                octave
                );
        }
        HttpResponse.println(F("</table>"));

        DSP.end();
    }
    else
        HttpResponse.println(F("DSP.begin failed!"));

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F("handleHttpSyncFTPRequest"));

    Html.writeHeader(F("FTP Sync"), true, true);

    HttpResponse.println(F("<div><pre class=\"ftplog\">"));
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println(F("</pre></div>"));

    if (success)
        HttpResponse.println(F("<p>Success!</p>"));
    else
        HttpResponse.println(F("<p>Failed!</p>"));
 
    Html.writeFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    Html.writeHeader(F("Event log"), true, true, REFRESH_INTERVAL);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.println(F("<p><a href=\"/events/clear\">Clear event log</a></p>"));

    Html.writeFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpEventLogClearRequest()
{
    Tracer tracer(F("handleHttpEventLogClearRequest"));

    EventLog.clear();
    logEvent(F("Event log cleared."));

    handleHttpEventLogRequest();
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

    WebServer.send(200, F("text/html"), HttpResponse);
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

    String tzOffset = WebServer.arg(CFG_TZ_OFFSET);

    PersistentData.timeZoneOffset = tzOffset.toInt();

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();

    WiFiSM.reset();
}


void handleHttpNotFound()
{
    TRACE(F("Unexpected HTTP request: %s\n"), WebServer.uri().c_str());
    WebServer.send(404, F("text/plain"), F("Unexpected request."));
}
