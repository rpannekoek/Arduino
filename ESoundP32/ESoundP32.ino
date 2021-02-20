#define DEBUG_ESP_PORT Serial

#include <math.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <esp_a2dp_api.h>
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
#include <BluetoothAudio.h>
#include <DSP32.h>
#include <I2SMicrophone.h>
#include <I2SDAC.h>
#include <WaveBuffer.h>
#include <FX.h>
#include "PersistentData.h"
#include "FXReverb.h"
#include "FXFlanger.h"
#include "FXModulation.h"
#include "FXFilter.h"
#include "FXLoop.h"

#define SAMPLE_FREQUENCY 44100
#define DSP_FRAME_SIZE 2048
#define WAVE_BUFFER_SAMPLES (15 * SAMPLE_FREQUENCY)
#define FULL_SCALE 32768
#define DB_MIN 32
#define RUN_DSP_INTERVAL 500
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#define COD_AUDIO_RENDERING (ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING)

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

#define REFRESH_INTERVAL 30

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
DSP32 DSP(/*tracePerformance*/ false);
BluetoothAudio BTAudio;
Adafruit_SSD1306 Display(DISPLAY_WIDTH, DISPLAY_HEIGHT, &SPI, TFT_DC, TFT_RST, TFT_CS);
WaveBuffer WaveBuffer;
FXEngine SoundEffects(WaveBuffer, SAMPLE_FREQUENCY, LED_BUILTIN);
I2SMicrophone Mic(
    SoundEffects,
    SAMPLE_FREQUENCY,
    I2S_NUM_1,
    /*bck*/GPIO_NUM_12,
    /*ws*/GPIO_NUM_15,
    /*data*/GPIO_NUM_13
    );
I2SDAC DAC(
    WaveBuffer,
    SAMPLE_FREQUENCY,
    I2S_NUM_0,
    /*bck*/GPIO_NUM_21,
    /*ws*/GPIO_NUM_32,
    /*data*/GPIO_NUM_22,
    /*timing*/GPIO_NUM_2
    );

WaveStats lastWaveStats;
float* lastOctavePower;
int16_t* dspBuffer;

time_t currentTime = 0;
bool isFTPEnabled = false;
time_t actionPerformedTime = 0;
uint32_t a2dpSamples = 0;
uint32_t lastBTSamples = 0;
TaskHandle_t micDataSinkTaskHandle;
uint32_t runDspMillis = 0;
uint32_t lastBTMillis = 0;
uint32_t lastMicMillis = 0;
uint32_t lastMicSamples = 0;
bool useMicAGC = false;



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


void logError(String errorMessage)
{
    String msg = F("ERROR: ");
    msg += errorMessage;
    WiFiSM.logEvent(msg);
}


bool checkError(esp_err_t err, String func)
{
    if (err == ESP_OK) return false;

    String msg = func;
    msg += " returned ";
    msg += String(err, 16); // hex
    logError(msg);

    return true;
}


void bootDisplay(const char* text = nullptr)
{
    Tracer tracer(F(__func__), text);

    Display.clearDisplay();
    Display.setTextColor(SSD1306_WHITE);

    Display.setTextSize(2);
    Display.setCursor(0, 0);
    Display.print(PersistentData.hostName);

    if (text != nullptr)
    {
        Display.setTextSize(1);
        Display.setCursor(0, 20);
        Display.print(text);
    }

    Display.display();
}


// Boot code
void setup() 
{
    // Turn built-in LED on during boot
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_ON);

    // GPIO2 is used for I2SDAC timing output
    pinMode(GPIO_NUM_2, OUTPUT);
    digitalWrite(GPIO_NUM_2, 0);

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

    if (Display.begin())
    {
        TRACE(F("Display size: %d x %d\n"), DISPLAY_WIDTH, DISPLAY_HEIGHT);
        bootDisplay();
    }
    else
        logError(F("Display initialization failed!"));

    START_SPIFFS;

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/bt", handleHttpBluetoothRequest);
    WebServer.on("/mic", HTTP_GET, handleHttpMicRequest);
    WebServer.on("/mic", HTTP_POST, handleHttpMicPostRequest);
    WebServer.on("/fx", HTTP_GET, handleHttpFxRequest);
    WebServer.on("/fx", HTTP_POST, handleHttpFxPostRequest);
    WebServer.on("/wave", handleHttpWaveRequest);
    WebServer.on("/wave/dsp", handleHttpWaveDspRequest);
    WebServer.on("/wave/ftp", handleHttpWaveFtpRequest);
    WebServer.on("/events", handleHttpEventLogRequest);
    WebServer.on("/config", HTTP_GET, handleHttpConfigFormRequest);
    WebServer.on("/config", HTTP_POST, handleHttpConfigFormPost);
    WebServer.serveStatic(ICON, SPIFFS, ICON, cacheControl);
    WebServer.serveStatic(CSS, SPIFFS, CSS, cacheControl);
    WebServer.onNotFound(handleHttpNotFound);

    WiFiSM.on(WiFiState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiState::Initialized, onWiFiInitialized);
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (!BTAudio.begin(PersistentData.hostName))
        logError(F("Starting Bluetooth failed"));

    if (!WaveBuffer.begin(WAVE_BUFFER_SAMPLES))
        logError(F("WaveBuffer.begin() failed"));

    dspBuffer = (int16_t*) ps_malloc(DSP_FRAME_SIZE * sizeof(int16_t));
    if (dspBuffer == nullptr)
        logError(F("Allocating DSP buffer failed"));

    if (!DSP.begin(DSP_FRAME_SIZE, WindowType::Hann, SAMPLE_FREQUENCY))
        logError(F("DSP.begin() failed"));

    if (!Mic.begin())
        logError(F("Starting microphone failed"));

    if (!DAC.begin())
        logError(F("Starting DAC failed"));

    SoundEffects.begin();
    SoundEffects.add(new FXLoop());
    SoundEffects.add(new FXReverb());
    SoundEffects.add(new FXFlanger());
    SoundEffects.add(new FXModulation());
    SoundEffects.add(new FXFilter());

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly (on Core #1)
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();
    WiFiSM.run();

    if ((runDspMillis != 0) && (millis() >= runDspMillis))
    {
        runDspMillis = millis() + RUN_DSP_INTERVAL;
        runWaveDsp();
        displayWaveInfo();
    }
    else 
        delay(10);
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();

    bootDisplay(WiFiSM.getIPAddress().c_str());
}


// Called repeatedly after WiFi is initialized (on Core #1)
void onWiFiInitialized()
{
}


void a2dpDataSink(const uint8_t* data, uint32_t length)
{   
    StereoData* stereoData = (StereoData*)data;
    uint32_t samples = length / sizeof(StereoData);
    for (int i = 0; i < samples; i++)
    {
        int32_t monoData = stereoData[i].right;
        monoData +=  stereoData[i].left;
        monoData /= 2;

        SoundEffects.addSample(monoData);
    }

    a2dpSamples += samples;
}


int32_t a2dpDataSource(uint8_t* data, int32_t length)
{
    if (length < 0) return -1; // Buffer flush request

    int samples = length / sizeof(StereoData);
    StereoData* stereoData = (StereoData*)data;
    int16_t* monoData = (int16_t*)data;

    WaveBuffer.getNewSamples(monoData, samples);

    for (int i = samples - 1; i >= 0; i--)
    {
        stereoData[i].left = monoData[i];
        stereoData[i].right = monoData[i];
    }

    a2dpSamples += samples;
    return length;
}


void runWaveDsp()
{
    Tracer tracer(F(__func__));

    lastWaveStats = WaveBuffer.getStatistics(SAMPLE_FREQUENCY / 2); // last 0.5s

    if (WaveBuffer.getNumSamples() < DSP_FRAME_SIZE)
    {
        lastOctavePower = nullptr;
        return;
    }

    WaveBuffer.getSamples(dspBuffer, DSP_FRAME_SIZE);
    complex_t* complexSpectrum = DSP.runFFT(dspBuffer);
    float* spectralPower = DSP.getSpectralPower(complexSpectrum);
    lastOctavePower = DSP.getOctavePower(spectralPower);
}


void displayWaveInfo()
{
    Tracer tracer(F(__func__));

    float dBFS = 20 * log10f(float(lastWaveStats.peak) / FULL_SCALE);

    Display.clearDisplay();
    drawVUMeter(dBFS, lastOctavePower, DISPLAY_WIDTH, DISPLAY_HEIGHT - 10);
    Display.setTextSize(1);
    Display.setCursor(0, DISPLAY_HEIGHT - 8);
    Display.printf(
        "%0.0f dB  %d%%",
        dBFS,
        WaveBuffer.getFillPercentage()
        );
    Display.display();
}


void drawVUMeter(float vu, float* octavePower, uint8_t width, uint8_t height)
{
    int vuBarSegments = width / 4;
    int vuBarWidth = 8;
    int vuSegments = vuBarSegments * (vu + DB_MIN) / DB_MIN;
    if (vuSegments < 0) vuSegments = 0;
    for (int s = 0; s < vuSegments; s++)
    {
        Display.fillRect(s * 4, 0, 3, vuBarWidth - 2, SSD1306_WHITE);
    }
    for (int s = vuSegments; s < vuBarSegments; s++)
    {
        Display.drawPixel(s * 4 + 1, (vuBarWidth / 2 - 1), SSD1306_WHITE);
    }

    // Octave bars
    if (octavePower != nullptr)
    {
        Display.drawRect(0, vuBarWidth, width, height - vuBarWidth, SSD1306_WHITE);

        float dBminOctave = DB_MIN * 2;
        int octaveBarWidth = (width - 2) / DSP.getOctaves();
        int octaveBarSegments = (height - vuBarWidth - 2) / 3;
        for (int o = 0; o < DSP.getOctaves(); o++)
        {
            int barX = 1 + o * octaveBarWidth;

            float dBoctave = 10 * log10f(octavePower[o]);
            int dBoctaveSegments = octaveBarSegments * (dBoctave + dBminOctave) / dBminOctave;
            if (dBoctaveSegments < 0) dBoctaveSegments = 0;
            for (int s = 0; s < dBoctaveSegments; s++)
            {
                int segmentY = height - 3 - (s * 3);
                Display.fillRect(barX, segmentY, octaveBarWidth - 1, 2, SSD1306_WHITE);
            }
            for (int s = dBoctaveSegments; s < octaveBarSegments; s++)
            {
                int segmentY = height - 2 - (s * 3);
                Display.drawPixel(barX + octaveBarWidth / 2, segmentY, SSD1306_WHITE);
            }
        }
    }
}


bool ftpWaveFile(Print* printTo)
{
    Tracer tracer(F(__func__));

    if (!isFTPEnabled)
    {
        logEvent(F("No FTP server configured.\n"));
        return false;
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "%s.wav", PersistentData.hostName);

    if (!FTPClient.begin(PersistentData.ftpServer, PersistentData.ftpUser, PersistentData.ftpPassword, FTP_DEFAULT_CONTROL_PORT, printTo))
    {
        FTPClient.end();
        return false;
    }

    bool success = false;
    int responseCode = FTPClient.sendCommand("TYPE", "I", true);
    if (responseCode == 200)
    {
        WiFiClient& dataClient = FTPClient.store(filename);
        if (dataClient.connected())
        {
            WaveBuffer.writeWaveFile(dataClient, SAMPLE_FREQUENCY);
            dataClient.stop();

            if (FTPClient.readServerResponse() == 226)
                success = true;
            else
                TRACE(F("FTP Store command failed: %s\n"), FTPClient.getLastResponse());
        }
    }
    else
        TRACE(F("FTP Type command failed: %s\n"), FTPClient.getLastResponse());

    FTPClient.end();

    return success;
}


bool shouldPerformAction(String name)
{
    if (!WebServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = WebServer.arg(name).toInt();

    if (actionTime == actionPerformedTime)
        return false; // Action already performed

    actionPerformedTime = actionTime;
    return true;
}


void handleHttpRootRequest()
{
    Tracer tracer(F(__func__));
    
    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    Html.writeHeader(F("Home"), false, false, REFRESH_INTERVAL);

    HttpResponse.println(F("<h1>Device Status</h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>Free IRAM</td><td>%u</td></tr>\r\n"), ESP.getFreeHeap());
    HttpResponse.printf(F("<tr><td>Free PSRAM</td><td>%u</td></tr>\r\n"), ESP.getFreePsram());
    HttpResponse.printf(
        F("<tr><td><a href=\"/wave\">Wave buffer</a></td><td>%u (%d %%)</td></tr>\r\n"),
        WaveBuffer.getNumSamples(),
        WaveBuffer.getFillPercentage()
        );
    HttpResponse.printf(F("<tr><td><a href=\"/events\">Events logged</a></td><td>%d</td></tr>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1><a href=\"/bt\">Bluetooth Status</a></h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>State</td><td>%s</td></tr>\r\n"), BTAudio.getStateName().c_str());
    HttpResponse.printf(F("<tr><td>Remote device</td><td>%s</td></tr>\r\n"), BTAudio.getRemoteDevice().c_str());
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%d</td></tr>\r\n"), BTAudio.getSampleRate());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1><a href=\"/mic\">Microphone Status</a></h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>Recording</td><td>%s</td></tr>\r\n"), Mic.isRecording() ? "Yes" : "No");
    HttpResponse.printf(F("<tr><td>Gain</td><td>%0.0f dB</td></tr>\r\n"), Mic.getGain());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1><a href=\"/fx\">Sound Effects<a></h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    for (int i = 0; i < SoundEffects.getNumRegisteredFX(); i++)
    {
        SoundEffect* soundEffectPtr = SoundEffects.getSoundEffect(i); 
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%s</td></tr>"),
            soundEffectPtr->getName().c_str(),
            soundEffectPtr->isEnabled() ? "Enabled" : "Disabled"
            );
    }
    HttpResponse.println(F("</table>"));
    
    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


bool handleBluetoothConnection(esp_bd_addr_t deviceAddress, int deviceIndex, int connIndex, bool allowConnect)
{
    if (deviceIndex == connIndex)
    {
        if (BTAudio.connectSource(deviceAddress, a2dpDataSource))
        {
            HttpResponse.println(F("Connecting..."));
            return true;
        }
        else
            HttpResponse.println(F("Failed."));
    }
    else if (allowConnect)
        HttpResponse.printf(F("<a href=\"?conn=%u&connIndex=%d\">Connect</a>\r\n"), currentTime, deviceIndex);

    return false;
}


void handleHttpBluetoothRequest()
{
    Tracer tracer(F(__func__));

    float a2dpSampleRateKHz = float(a2dpSamples - lastBTSamples) / (millis() - lastBTMillis);
    lastBTSamples = a2dpSamples;
    lastBTMillis = millis();

    Html.writeHeader(F("Bluetooth"), true, true, 5);

    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>State</td><td>%s</td></tr>\r\n"), BTAudio.getStateName().c_str());
    HttpResponse.printf(F("<tr><td>Remote device</td><td>%s</td></tr>\r\n"), BTAudio.getRemoteDevice().c_str());
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%d</td></tr>\r\n"), BTAudio.getSampleRate());
    HttpResponse.printf(F("<tr><td>A2DP Samples</td><td>%u</td></tr>\r\n"), a2dpSamples);
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%0.2f kHz</td></tr>\r\n"), a2dpSampleRateKHz);
    HttpResponse.println(F("</table>"));

    if (shouldPerformAction(F("audio")))
    {
        esp_a2d_media_ctrl_t ctrl = (esp_a2d_media_ctrl_t) WebServer.arg(F("ctrl")).toInt();
        if (BTAudio.mediaControl(ctrl))
            HttpResponse.println(F("<p>Performing media control...</p>"));
        else
            HttpResponse.println(F("<p>Media control failed.</p>"));
    }
    else
    {
        switch (BTAudio.getState())
        {
            case BluetoothState::AudioConnected:
            case BluetoothState::AudioSuspended:
            case BluetoothState::AudioStopped:
                HttpResponse.printf(
                    F("<p><a href=\"?audio=%u&ctrl=%d\">Start Audio</a></p>\r\n"),
                    currentTime,
                    ESP_A2D_MEDIA_CTRL_START
                    );
                break;
            
            case BluetoothState::AudioStarted:
                HttpResponse.printf(
                    F("<p><a href=\"?audio=%u&ctrl=%d\">Stop Audio</a></p>\r\n"),
                    currentTime,
                    ESP_A2D_MEDIA_CTRL_STOP
                    );
                break;
        }
    }

    int connIndex = shouldPerformAction(F("conn")) ? WebServer.arg(F("connIndex")).toInt() : 0;
    bool allowConnect = (BTAudio.isSinkStarted() || BTAudio.isSourceStarted()) ? false :
        (BTAudio.getState() == BluetoothState::Initialized) || 
        (BTAudio.getState() == BluetoothState::DiscoveryComplete) ||
        (BTAudio.getState() == BluetoothState::AudioDisconnected); 

    if (shouldPerformAction(F("startSink")))
    {
        if (BTAudio.startSink(a2dpDataSink))
        {
            allowConnect = false;
            runDspMillis = millis();
            HttpResponse.println(F("<p>Sink started.</p>\r\n"));
        }
        else
            HttpResponse.println(F("<p>Starting sink failed.</p>\r\n"));
    }
    else if (allowConnect)
        HttpResponse.printf(F("<p><a href=\"?startSink=%u\">Start sink</a></p>\r\n"), currentTime);

    if (shouldPerformAction(F("stopSink")))
    {
        if (BTAudio.stopSink())
        {
            HttpResponse.println(F("<p>Sink stopped.</p>\r\n"));
        }
        else
            HttpResponse.println(F("<p>Stopping sink failed.</p>\r\n"));
    }
    else if (BTAudio.isSinkStarted())
        HttpResponse.printf(F("<p><a href=\"?stopSink=%u\">Stop sink</a></p>\r\n"), currentTime);

    if (BTAudio.isSourceStarted())
    {
        if (shouldPerformAction(F("disconnect")) || BTAudio.getState() == BluetoothState::AudioDisconnected)
        {
            if (BTAudio.disconnectSource())
                HttpResponse.println(F("<p>Disconnecting...</p>\r\n"));
            else
                HttpResponse.println(F("<p>Disconnect failed.</p>\r\n"));
        }
        else
            HttpResponse.printf(F("<p><a href=\"?disconnect=%u\">Disconnect</a></p>\r\n"), currentTime);
    }

    HttpResponse.println(F("<h2>Stored devices</h2>"));
    HttpResponse.println(F("<table class=\"btDevices\""));
    HttpResponse.println(F("<tr><th>Name</th><th>Address</th><th></th></tr>"));
    if (PersistentData.btSinkName[0] != 0)
    {
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%s</td><td>"),
            PersistentData.btSinkName,
            BluetoothAudio::formatDeviceAddress(PersistentData.btSinkAddress)
            );
        handleBluetoothConnection(PersistentData.btSinkAddress, -1, connIndex, allowConnect);
        HttpResponse.println(F("</td></tr>"));
    }
    HttpResponse.println("</table>");

    HttpResponse.println(F("<h2>Discovered devices</h2>"));
    HttpResponse.println(F("<table class=\"btDevices\""));
    HttpResponse.println(F("<tr><th>Name</th><th>Address</th><th>COD</th><th>Device</th><th>Service</th><th>RSSI</th><th></th></tr>"));
    int i = 1;
    BluetoothDeviceInfo* devInfoPtr = BTAudio.discoveredDevices.getFirstEntry();
    while (devInfoPtr != nullptr)
    {
        HttpResponse.printf(
            F("<tr><td>%s</td><td>%s</td><td>%X</td><td>%X</td><td>%X</td><td>%d</td><td>"),
            devInfoPtr->deviceName.c_str(),
            BluetoothAudio::formatDeviceAddress(devInfoPtr->deviceAddress),
            devInfoPtr->cod,
            devInfoPtr->codMajorDevice,
            devInfoPtr->codServices,
            devInfoPtr->rssi
            );
        if (handleBluetoothConnection(
            devInfoPtr->deviceAddress,
            i,
            connIndex,
            allowConnect && ((devInfoPtr->codServices & COD_AUDIO_RENDERING) == COD_AUDIO_RENDERING))
            )
        {
            memcpy(PersistentData.btSinkAddress, devInfoPtr->deviceAddress, sizeof(esp_bd_addr_t));
            strncpy(PersistentData.btSinkName, devInfoPtr->deviceName.c_str(), sizeof(PersistentData.btSinkName));
            PersistentData.validate();
            PersistentData.writeToEEPROM();
        }
        HttpResponse.println(F("</td></tr>"));

        devInfoPtr = BTAudio.discoveredDevices.getNextEntry();
        i++;
    }
    HttpResponse.println("</table>");

    if (!BTAudio.isSinkStarted() && !BTAudio.isSourceStarted() && BTAudio.getState() != BluetoothState::Discovering)
    {
        if (shouldPerformAction(F("disco")))
        {
            if (BTAudio.startDiscovery(4))
                HttpResponse.println(F("<p>Discovery started...</p>"));
            else
                HttpResponse.println(F("<p>Starting discovery failed.</p>"));
        }
        else
            HttpResponse.printf(F("<p><a href=\"?disco=%u\">Start discovery</a></p>"), currentTime);
    }

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpMicRequest()
{
    Tracer tracer(F(__func__));

    if (!Mic.isRecording()) Mic.startRecording();
    runDspMillis = 0;
    const int refreshInterval = 3;

    WaveStats waveStats = WaveBuffer.getStatistics(SAMPLE_FREQUENCY * refreshInterval);
    float dBFS = 20 * log10f(float(waveStats.peak) / FULL_SCALE);
    float vuBar = (dBFS + DB_MIN) / DB_MIN;
    if (vuBar < 0) vuBar = 0;

    float micGain;
    if (useMicAGC)
        micGain = Mic.adjustGain(dBFS);
    else
        micGain = Mic.getGain();

    float micSampleRateKHz = float(Mic.getRecordedSamples() - lastMicSamples) / (millis() - lastMicMillis);
    lastMicSamples = Mic.getRecordedSamples();
    lastMicMillis = millis();

    Html.writeHeader(F("Microphone"), true, true, refreshInterval);

    HttpResponse.println(F("<form method=\"POST\">"));
    HttpResponse.println(F("<table>"));

    HttpResponse.println(F("<tr><td>Level</td><td>"));
    Html.writeBar(vuBar, F("deliveredBar"), true);
    HttpResponse.print(F("<div>"));
    Html.writeBar(vuBar, F("emptyBar"), false, false);
    HttpResponse.printf(F("%0.0f dB</div>"), dBFS);
    HttpResponse.println(F("</td></tr>"));

    HttpResponse.printf(F("<tr><td>Clipped</td><td>%u</td></tr>\r\n"), WaveBuffer.getNumClippedSamples());

    Html.writeSlider(F("Gain"), F("Gain"), F("dB"), roundf(micGain), 0, 48);

    Html.writeCheckbox(F("AGC"), F("AGC"), useMicAGC);

    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%0.2f kHz</td></tr>\r\n"), micSampleRateKHz);
    HttpResponse.printf(
        F("<tr><td>Cycles</td><td>%u (%0.1f us)</td></tr>\r\n"), 
        Mic.getCycles(),
        float(Mic.getCycles()) / ESP.getCpuFreqMHz()
        );

    HttpResponse.println(F("</table>"));
    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpMicPostRequest()
{
    Tracer tracer(F(__func__));

    useMicAGC = WebServer.arg(F("AGC")) == F("true");
    if (!useMicAGC)
    {
        int micGain = WebServer.arg(F("Gain")).toInt();
        Mic.setGain(micGain);
    }

    handleHttpMicRequest();
}


void handleHttpFxRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Sound Effects"), true, true);

    HttpResponse.println(F("<form method=\"POST\">"));
 
    for (int i = 0; i < SoundEffects.getNumRegisteredFX(); i++)
    {
        SoundEffect* fxPtr = SoundEffects.getSoundEffect(i);
        HttpResponse.printf(F("<h2>%s</h2>\r\n"), fxPtr->getName().c_str());
        HttpResponse.println(F("<table>"));
        Html.writeCheckbox(fxPtr->getName(), F("Enable"), fxPtr->isEnabled());
        fxPtr->writeConfigForm(Html);
        HttpResponse.println(F("</table>"));
    }

    HttpResponse.println(F("<input type=\"submit\">"));
    HttpResponse.println(F("</form>"));

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpFxPostRequest()
{
    Tracer tracer(F(__func__));

    SoundEffects.reset();

    for (int i = 0; i < SoundEffects.getNumRegisteredFX(); i++)
    {
        SoundEffect* fxPtr = SoundEffects.getSoundEffect(i);
        fxPtr->handleConfigPost(WebServer);
        if (WebServer.hasArg(fxPtr->getName()))
            SoundEffects.enable(fxPtr);
    }

    handleHttpFxRequest();
}



void writeHtmlSampleDump(const int16_t* buffer, size_t size)
{
    HttpResponse.print(F("<pre class=\"sampleDump\">"));
    for (int i = 0; i < size; i++)
    {
        if ((i % 16) == 0) HttpResponse.println("");
        HttpResponse.printf(F("%06d "), buffer[i]);
    }
    HttpResponse.println(F("</pre>"));
}


void testFillWaveBuffer()
{
    Tracer tracer(F(__func__));

    String waveform = WebServer.arg(F("waveform"));

    WaveBuffer.clear();
    if (waveform == F("sin"))
    {
        // Generate sine wave signal @ Fs/64 with small DC offset
        for (int i = 0; i < SAMPLE_FREQUENCY; i++)
        {
            float phi = float(2 * PI) * i / 64;
            int16_t sinewave = sinf(phi) * 32000 + 700;
            SoundEffects.addSample(sinewave);
        }
    }
    else
    {
        // Generate square wave signal @ Fs/64 with small DC offset
        for (int i = 0; i < SAMPLE_FREQUENCY; i++)
        {
            int16_t squarewave = ((i % 64) < 32) ? 32000 : -32000;
            squarewave += 700;
            SoundEffects.addSample(squarewave);
        }
    }
}

void handleHttpWaveRequest()
{
    Tracer tracer(F(__func__));

    if (shouldPerformAction(F("clear")))
        WaveBuffer.clear();

    if (shouldPerformAction(F("startMic")))
        Mic.startRecording();

    if (shouldPerformAction(F("stopMic")))
        Mic.stopRecording();

    if (shouldPerformAction(F("startDAC")))
        DAC.startPlaying();

    if (shouldPerformAction(F("stopDAC")))
        DAC.stopPlaying();

    if (shouldPerformAction(F("test")))
        testFillWaveBuffer();

    if (shouldPerformAction(F("startVU")))
        runDspMillis = millis();

    if (shouldPerformAction(F("stopVU")))
        runDspMillis = 0;

    bool isRecording = Mic.isRecording() || BTAudio.isSinkStarted();
    uint16_t refreshInterval = isRecording ? 2 : 0;

    Html.writeHeader(F("Wave Buffer"), true, true, refreshInterval);

    if (WaveBuffer.getNumSamples() != 0)
        HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear buffer</a></p>\r\n"), currentTime);

    if (runDspMillis == 0)
        HttpResponse.printf(F("<p><a href=\"?startVU=%u\">Start VU Meter</a></p>\r\n"), currentTime);
    else
        HttpResponse.printf(F("<p><a href=\"?stopVU=%u\">Stop VU Meter</a></p>\r\n"), currentTime);

    if (Mic.isRecording())
        HttpResponse.printf(F("<p><a href=\"?stopMic=%u\">Stop microphone</a></p>\r\n"), currentTime);

    if (!isRecording)
    {
        HttpResponse.printf(F("<p><a href=\"?startMic=%u\">Start microphone</a></p>\r\n"), currentTime);
        HttpResponse.printf(F("<p><a href=\"?test=%u\">Test fill with squarewave</a></p>\r\n"), currentTime);
        HttpResponse.printf(F("<p><a href=\"?test=%u&waveform=sin\">Test fill with sinewave</a></p>\r\n"), currentTime);
        HttpResponse.println(F("<p><a href=\"/wave/dsp\">DSP</a></p>"));
        if (isFTPEnabled)
            HttpResponse.println(F("<p><a href=\"/wave/ftp\">Write to FTP Server</a></p>"));
    }

    if (DAC.isPlaying())
        HttpResponse.printf(F("<p><a href=\"?stopDAC=%u\">Stop DAC</a></p>\r\n"), currentTime);
    else if (!BTAudio.isSourceStarted())
        HttpResponse.printf(F("<p><a href=\"?startDAC=%u\">Start DAC</a></p>\r\n"), currentTime);

    WaveStats waveStats = WaveBuffer.getStatistics(); // Get stats for whole buffer

    HttpResponse.println(F("<table class=\"waveStats\">"));
    HttpResponse.printf(
        F("<tr><th>Samples</th><td>%u (%d %%)</td></tr>\r\n"),
        WaveBuffer.getNumSamples(),
        WaveBuffer.getFillPercentage()
        );
    HttpResponse.printf(
        F("<tr><th>Clipped samples</th><td>%u</td></tr>\r\n"),
         WaveBuffer.getNumClippedSamples()
         );
    HttpResponse.printf(
        F("<tr><th>New samples</th><td>%u (%d ms)</td></tr>\r\n"),
        WaveBuffer.getNumNewSamples(),
        1000 * WaveBuffer.getNumNewSamples() / SAMPLE_FREQUENCY
        );
    HttpResponse.printf(
        F("<tr><th>Cycles</th><td>%u (%0.2f us)</td></tr>\r\n"),
        WaveBuffer.getCycles(),
        float(WaveBuffer.getCycles()) / ESP.getCpuFreqMHz()
        );
    HttpResponse.printf(
        F("<tr><th>Peak</th><td>%d (%0.0f dBFS)</td></tr>\r\n"),
        waveStats.peak,
        20 * log10f(float(waveStats.peak) / FULL_SCALE)
        );
    HttpResponse.printf(
        F("<tr><th>Average</th><td>%0.0f (%0.0f dBFS)</td></tr>\r\n"),
        waveStats.average,
        20 * log10f(waveStats.average / FULL_SCALE)
        );
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h2>Hex dump (last 128 samples)</h2>"));
    size_t numSamples = WaveBuffer.getSamples(dspBuffer, 128);
    writeHtmlSampleDump(dspBuffer, numSamples);

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void writeHtmlDspResult()
{
    Tracer tracer(F(__func__));

    WaveBuffer.getSamples(dspBuffer, DSP_FRAME_SIZE);
    complex_t* complexSpectrum = DSP.runFFT(dspBuffer);
    float* spectralPower = DSP.getSpectralPower(complexSpectrum);
    float* octavePower = DSP.getOctavePower(spectralPower);
    BinInfo fundamental = DSP.getFundamental(spectralPower);
    String note = DSP.getNote(fundamental.getCenterFrequency());

    // Output fundamental info
    HttpResponse.println(F("<h2>Fundamental analysis</h2>"));
    HttpResponse.printf(
        F("<p>bin #%i => %0.0f - %0.0f Hz, Center = %0.0f Hz</p>\r\n"),
        fundamental.index,
        fundamental.minFrequency,
        fundamental.maxFrequency,
        fundamental.getCenterFrequency()
        );
    HttpResponse.printf(
        F("<p>Note: %s - %s, Center = %s</p>\r\n"),
        DSP.getNote(fundamental.minFrequency).c_str(),
        DSP.getNote(fundamental.maxFrequency).c_str(),
        note.c_str()
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

        float bar = (dB + 60) / 60;
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


void handleHttpWaveDspRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Wave DSP"), true, true);

    if (WaveBuffer.getNumSamples() < DSP_FRAME_SIZE)
        HttpResponse.println(F("Not enough data in wave buffer."));
    else
        writeHtmlDspResult();

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpWaveFtpRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("FTP Wave File"), true, true);

    HttpResponse.println(F("<div><pre class=\"ftplog\">"));
    bool success = ftpWaveFile(&HttpResponse); 
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
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Event log"), true, true, REFRESH_INTERVAL);

    if (shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        logEvent(F("Event log cleared."));
    }

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        HttpResponse.printf(F("<div>%s</div>\r\n"), event);
        event = EventLog.getNextEntry();
    }

    HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear event log</a></p>\r\n"), currentTime);

    Html.writeFooter();

    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

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
    Tracer tracer(F(__func__));

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
