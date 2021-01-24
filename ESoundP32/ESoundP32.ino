#define DEBUG_ESP_PORT Serial

#include <math.h>
#include <U8g2lib.h>
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
#include <PSRAM.h>
#include "PersistentData.h"
#include "DSP32.h"

#define SAMPLE_FREQUENCY 44100
#define I2S_FRAME_SIZE 256
#define DSP_FRAME_SIZE 2048
#define SAMPLE_BUFFER_SIZE (DSP_FRAME_SIZE * sizeof(int16_t))
#define I2S_PORT_MIC I2S_NUM_0
#define COD_AUDIO_RENDERING (ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING)

#define ICON "/apple-touch-icon.png"
#define CSS "/styles.css"

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
BluetoothAudio BTAudio;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C Display(U8G2_R0, /*RST*/ U8X8_PIN_NONE, /*SCL*/ GPIO_NUM_4, /*SDA*/ GPIO_NUM_5);   
DSP32 DSP(true); // Trace DSP performance

const i2s_config_t I2SMicConfig = 
{
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_FREQUENCY,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // high interrupt priority
    .dma_buf_count = 2,
    .dma_buf_len = I2S_FRAME_SIZE, // samples
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0   
};
    
const i2s_pin_config_t I2SMicPinConfig =
{
    .bck_io_num = GPIO_NUM_12,
    .ws_io_num = GPIO_NUM_13,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = GPIO_NUM_14
};

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;
bool isFTPEnabled = false;
uint8_t displayWidth =0 ;
uint8_t displayHeight =0;
bool sampleBTAudio = false;
size_t volatile btAudioBytes = 0;
int16_t* sampleBufferPtr = nullptr;
time_t btActionTime = 0;
uint16_t a2dpPackets = 0;

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


void bootDisplay(const char* text = nullptr)
{
    Tracer tracer(F(__func__), text);

    if (displayWidth == 0) return;

    Display.clearBuffer();

    Display.setFont(u8g2_font_10x20_tf);
    Display.setCursor(0, 20);
    Display.print(PersistentData.hostName);

    if (text != nullptr)
    {
        Display.setFont(u8g2_font_8x13_tf);
        Display.setCursor(0, 35);
        Display.print(text);
    }

    Display.sendBuffer();
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
    WebServer.on("/bt", handleHttpBluetoothRequest);
    WebServer.on("/testdsp", handleHttpTestDSPRequest);
    WebServer.on("/testmic", handleHttpTestMicRequest);
    WebServer.on("/testa2dp", handleHttpTestA2DPRequest);
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

    // Start microphone I2S
    esp_err_t i2s_result = i2s_driver_install(I2S_PORT_MIC, &I2SMicConfig, 0, nullptr);
    if (i2s_result != ESP_OK)
        TRACE(F("I2S initialization failed: %X\n"), i2s_result);
    i2s_result = i2s_set_pin(I2S_PORT_MIC, &I2SMicPinConfig);
    if (i2s_result != ESP_OK)
        TRACE(F("Setting I2S pins failed: %X\n"), i2s_result);
    i2s_result = i2s_start(I2S_PORT_MIC);
    if (i2s_result != ESP_OK)
        TRACE(F("Starting I2S failed: %X\n"), i2s_result);

    //DSP.begin(DSP_FRAME_SIZE, WindowType::None, SAMPLE_FREQUENCY);

    if (Display.begin())
    {
        displayWidth = Display.getDisplayWidth();
        displayHeight = Display.getDisplayHeight();
        TRACE(F("Display size: %d x %d\n"), displayWidth, displayHeight);
        bootDisplay();
    }
    else
        TRACE(F("Display initialization failed!\n"));

    if (BTAudio.begin(PersistentData.hostName))
    {
        //if (!BTAudio.startSink(a2dpDataSink))
        //    TRACE(F("Failed starting Bluetooth A2DP Sink.\n"));
    }

    sampleBufferPtr = (int16_t*) ESP_MALLOC(SAMPLE_BUFFER_SIZE);

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();
    WiFiSM.run();
    delay(10);
}


void a2dpDataSink(const uint8_t* data, uint32_t length)
{
    if (sampleBTAudio)
    {
        btAudioBytes = std::min(length, SAMPLE_BUFFER_SIZE);
        memcpy(sampleBufferPtr, data, btAudioBytes);
        sampleBTAudio = false;
    }
}


int32_t a2dpDataSource(uint8_t* data, int32_t length)
{
    if (length > 0)
    {
        StereoData* stereoData = (StereoData*)data;
        // Find integer number of cycles approx. 1 kHz:
        uint32_t samples = length / sizeof(StereoData);
        uint32_t cycles = samples / 44;
        int period1 = samples / cycles;
        int period2 = period1 / 2;
        for (int i = 0; i < samples ; i++)
        {
            int squareWave1 = (i % period1) < (period1/2) ? 512 : -512;
            int squareWave2 = (i % period2) < (period2/2) ? 256 : -256;
            stereoData[i].left = squareWave1;
            stereoData[i].right = squareWave2;
        }

        if (++a2dpPackets % 100 == 1)
            TRACE(
                F("a2dpDataSource(%d) called %u times. %d samples. Periods: %d, %d\n"), 
                length,
                a2dpPackets,
                samples,
                period1,
                period2
                );
    }

    return length;
}


void onTimeServerSynced()
{
    currentTime = WiFiSM.getCurrentTime();

    bootDisplay(WiFiSM.getIPAddress().c_str());
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


void drawVUMeter(float* octavePower, uint8_t octaves, uint8_t width, uint8_t height)
{
    Tracer tracer(F(__func__));

    uint8_t barWidth = (width - 2) / octaves;
    uint8_t segments = (height - 2) / 3;
    const float minDb = -20;
    const float maxDb = 0;

    Display.drawFrame(0, 0, width, height);

    for (uint8_t o = 0; o < octaves; o++)
    {
        int barX = 1 + o * barWidth;

        float dB = std::min(std::max(10 * log10f(octavePower[o]), minDb), maxDb);
        uint8_t barSegments = roundf(segments * (dB - minDb) / (maxDb - minDb));
        for (int s = 0; s < barSegments; s++)
        {
            int segmentY = height - 3 - (s * 3);
            Display.drawBox(barX, segmentY, barWidth - 1, 2);
        }
    }
}

bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F(__func__));

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
    Tracer tracer(F(__func__));
    
    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    Html.writeHeader(F("Home"), false, false, REFRESH_INTERVAL);

    HttpResponse.println(F("<h1>Device Status</h1>"));
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

    HttpResponse.println(F("<h1>Bluetooth Status</h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>State</td><td>%s</td></tr>\r\n"), BTAudio.getStateName().c_str());
    HttpResponse.printf(F("<tr><td>Remote device</td><td>%s</td></tr>\r\n"), BTAudio.getRemoteDevice().c_str());
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%d</td></tr>\r\n"), BTAudio.getSampleRate());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p><a href=\"/bt\">Bluetooth</a></p>"));
    HttpResponse.println(F("<p><a href=\"/testdsp\">Test DSP</a></p>"));
    HttpResponse.println(F("<p><a href=\"/testmic\">Test Microphone</a></p>"));
    HttpResponse.println(F("<p><a href=\"/testa2dp\">Test Bluetooth Audio</a></p>"));

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


bool shouldPerformAction(String name)
{
    
    if (!WebServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = WebServer.arg(name).toInt();

    if (btActionTime == actionTime)
        return false; // Action already performed

    btActionTime = actionTime;
    return true;
}


void handleHttpBluetoothRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Bluetooth"), true, true, 5);
    HttpResponse.printf(F("<p>State: %s</p>\r\n"), BTAudio.getStateName().c_str());

    if (BTAudio.getState() != BluetoothState::Discovering)
    {
        bool discoStarted = false;
        if (shouldPerformAction(F("disco")))
        {
            if (BTAudio.startDiscovery())
            {
                discoStarted = true;
                HttpResponse.println(F("<p>Discovery started...</p>"));
            }
            else
                HttpResponse.println(F("<p>Starting discovery failed.</p>"));
        }
        if (!discoStarted)
            HttpResponse.printf(F("<p><a href=\"?disco=%u\">Start discovery</a></p>"), currentTime);
    }

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

    int connIndex = shouldPerformAction(F("conn")) ? WebServer.arg(F("connIndex")).toInt() : -1;

    HttpResponse.println(F("<h2>Discovered devices</h2>"));
    HttpResponse.println(F("<table class=\"btDevices\""));
    HttpResponse.println(F("<tr><th>Name</th><th>Address</th><th>COD</th><th>Device</th><th>Service</th><th>RSSI</th><th></th></tr>"));
    int i = 0;
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

        if (i == connIndex)
        {
            if (BTAudio.connectSource(devInfoPtr->deviceAddress, a2dpDataSource))
                HttpResponse.print(F("Connecting..."));
            else
                HttpResponse.print(F("Connect failed."));
        }
        else if ((BTAudio.getState() == BluetoothState::DiscoveryComplete) &&
            ((devInfoPtr->codServices & COD_AUDIO_RENDERING) == COD_AUDIO_RENDERING))
            HttpResponse.printf(F("<a href=\"?conn=%u&connIndex=%d\">Connect</a>"), currentTime, i);

        HttpResponse.println(F("</td></tr>"));

        devInfoPtr = BTAudio.discoveredDevices.getNextEntry();
        i++;
    }
    HttpResponse.println("</table>");

    Html.writeFooter();

    WebServer.send(200, "text/html", HttpResponse);
}


void handleHttpTestDSPRequest()
{
    Tracer tracer(F(__func__));

    String frameSizeString = WebServer.arg("frameSize");
    String windowTypeString = WebServer.arg("windowType");
    String sampleFrequencyString = WebServer.arg("fs");

    int frameSize = DSP_FRAME_SIZE;
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
        int16_t* signal = new int16_t[frameSize];
        for (int i = 0; i < frameSize; i++)
        {
            signal[i] = ((i % 64) < 32) ? 32000 : -32000;
            signal[i] += 700; 
        }

        complex_t* complexSpectrum = DSP.runFFT(signal);
        float* spectralPower = DSP.getSpectralPower(complexSpectrum);
        float* octavePower = DSP.getOctavePower(spectralPower);
        BinInfo fundamental = DSP.getFundamental(spectralPower);
        String note = DSP.getNote(fundamental.getCenterFrequency());

        if (displayWidth != 0)
        {
            // Show info on OLED Display
            Display.clearBuffer();
            drawVUMeter(octavePower, DSP.getOctaves(), displayWidth, 48);
            Display.setFont(u8g2_font_9x15_tf);
            Display.setCursor(0, 63);
            Display.printf("%0.0f Hz ~ %s", fundamental.getCenterFrequency(), note.c_str());
            Display.sendBuffer();
        }

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

        delete[] signal;
        DSP.end();
    }
    else
        HttpResponse.println(F("DSP.begin failed!"));

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void writeHtmlHexDump(uint8_t* buffer, size_t size)
{
    HttpResponse.print(F("<pre class=\"hexDump\">"));
    for (int i = 0; i < size; i++)
    {
        if ((i % 8) == 0) HttpResponse.print(" ");
        if ((i % 16) == 0) HttpResponse.println("");
        HttpResponse.printf(F("%2X "), buffer[i]);
    }
    HttpResponse.println(F("</pre>"));
}


void handleHttpTestMicRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("I2S Microphone Test"), true, true);

    size_t bytesToRead = I2S_FRAME_SIZE * sizeof(int32_t);
    size_t bytesRead;
    esp_err_t i2s_result = i2s_read(I2S_PORT_MIC, sampleBufferPtr, bytesToRead, &bytesRead, 100);
    if (i2s_result == ESP_OK)
    {
        HttpResponse.println(F("<h2>I2S buffer</h2>"));
        HttpResponse.printf(F("Read %d of %d bytes:\r\n"), bytesRead, bytesToRead);
        writeHtmlHexDump((uint8_t*)sampleBufferPtr, bytesRead);
    }
    else
    {
        HttpResponse.printf(F("i2s_read returned an error: %X\r\n"), i2s_result);
    }

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpTestA2DPRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader(F("Bluetooth Audio Test"), true, true);

    sampleBTAudio = true;
    int timeout = 0;
    while ((sampleBTAudio == true) && timeout++ < 10)
    {
        delay(10);
    }

    if (sampleBTAudio == false)
    {
        HttpResponse.println(F("<h2>A2DP data</h2>"));
        HttpResponse.printf(F("Read %d bytes:\r\n"), btAudioBytes);
        writeHtmlHexDump((uint8_t*)sampleBufferPtr, btAudioBytes);
    }
    else
    {
        sampleBTAudio = false;
        HttpResponse.printf(F("Timeout receiving Bluetooth audio\r\n"));
    }

    Html.writeFooter();
    WebServer.send(200, F("text/html"), HttpResponse);
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer(F(__func__));

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
    Tracer tracer(F(__func__));

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
    Tracer tracer(F(__func__));

    EventLog.clear();
    logEvent(F("Event log cleared."));

    handleHttpEventLogRequest();
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
