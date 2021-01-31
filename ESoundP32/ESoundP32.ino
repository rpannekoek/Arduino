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
#include "PersistentData.h"
#include "DSP32.h"
#include "WaveBuffer.h"

#define SAMPLE_FREQUENCY 44100
#define I2S_FRAME_SIZE 256
#define DSP_FRAME_SIZE 2048
#define WAVE_BUFFER_SAMPLES (15 * SAMPLE_FREQUENCY)

#define I2S_PORT_MIC I2S_NUM_0
#define FULL_SCALE 32768
#define DB_MIN 32
#define DISPLAY_WAVE_INFO_INTERVAL 500
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
BluetoothAudio BTAudio;
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C Display(U8G2_R0, /*RST*/ U8X8_PIN_NONE, /*SCL*/ GPIO_NUM_4, /*SDA*/ GPIO_NUM_5);
U8G2_SSD1327_MIDAS_128X128_F_HW_I2C Display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /*SCL*/ GPIO_NUM_21, /*SDA*/ GPIO_NUM_22);
//U8G2_SSD1327_MIDAS_128X128_F_SW_I2C Display(U8G2_R0, /*SCL*/ GPIO_NUM_21, /*SDA*/ GPIO_NUM_22, /* reset=*/ U8X8_PIN_NONE);
DSP32 DSP(/* tracePerformance: */ false);
WaveBuffer WaveBuffer;
int16_t* dspBuffer;

const i2s_config_t I2SMicConfig = 
{
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_FREQUENCY,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // low interrupt priority
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
bool isFTPEnabled = false;
uint8_t displayWidth =0 ;
uint8_t displayHeight =0;
time_t actionPerformedTime = 0;
uint32_t a2dpBytes = 0;
TaskHandle_t micDataSinkTaskHandle;
uint32_t displayWaveInfoMillis = 0;
volatile bool recordMic = false;
int16_t micScale = 4096;


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

    if (Display.begin())
    {
        Display.setBusClock(400000);
        displayWidth = Display.getDisplayWidth();
        displayHeight = Display.getDisplayHeight();
        TRACE(F("Display size: %d x %d\n"), displayWidth, displayHeight);
        bootDisplay();
    }
    else
        TRACE(F("Display initialization failed!\n"));

    START_SPIFFS;

    const char* cacheControl = "max-age=86400, public";
    WebServer.on("/", handleHttpRootRequest);
    WebServer.on("/bt", handleHttpBluetoothRequest);
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

    if (!BTAudio.begin(PersistentData.hostName))
        TRACE(F("Starting Bluetooth failed\n"));

    if (!WaveBuffer.begin(WAVE_BUFFER_SAMPLES))
        TRACE(F("WaveBuffer.begin() failed\n"));

    dspBuffer = (int16_t*) ps_malloc(DSP_FRAME_SIZE * sizeof(int16_t));
    if (dspBuffer == nullptr)
        TRACE(F("Allocating DSP buffer failed\n"));

    if (!DSP.begin(DSP_FRAME_SIZE, WindowType::Hann, SAMPLE_FREQUENCY))
    {
        TRACE(F("DSP.begin() failed\n"));
        return;
    }

    if (!spawnMicDataSink())
        TRACE(F("Spawning mic data sink failed\n"));

    Tracer::traceFreeHeap();

    digitalWrite(LED_BUILTIN, LED_OFF);
}


// Called repeatedly (on Core #1)
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();
    WiFiSM.run();

    if ((displayWaveInfoMillis != 0) && (millis() >= displayWaveInfoMillis))
    {
        displayWaveInfoMillis = millis() + DISPLAY_WAVE_INFO_INTERVAL;
        displayWaveInfo();
    }

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



void micDataSink(void* taskParams)
{
    Tracer tracer(F(__func__));

    while (true)
    {
        int32_t micSample;
        size_t bytesRead;
        esp_err_t err = i2s_read(I2S_PORT_MIC, &micSample, sizeof(int32_t), &bytesRead, 1);
        if (err != ESP_OK)
        {
            String message = F("i2s_read() returned error ");
            message += err;
            logEvent(message);
            break;
        }

        if (recordMic)
        {
            micSample /= micScale; // 32->16 bits
            WaveBuffer.addSample(micSample);
        }
    }
}


bool spawnMicDataSink()
{
    Tracer tracer(F(__func__));

    xTaskCreatePinnedToCore(
        micDataSink,
        "Mic Data Sink",
        8192, // Stack Size (words)
        nullptr, // taskParams
        3, // Priority
        &micDataSinkTaskHandle,
        PRO_CPU_NUM // Core ID
        );

    delay(100);

    return (micDataSinkTaskHandle != nullptr);
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

        if (WaveBuffer.isFull()) break;
        WaveBuffer.addSample(monoData);
    }

    a2dpBytes += length;
}


int32_t a2dpDataSource(uint8_t* data, int32_t length)
{
    if (length < 0)
    {
        TRACE(F("A2DP flush buffer requested.\n"));
        return -1;
    }

    int samples = length / sizeof(StereoData);
    StereoData* stereoData = (StereoData*)data;
    int16_t* monoData = (int16_t*)data;

    samples = WaveBuffer.getSamples(monoData, samples);

    for (int i = samples - 1; i >= 0; i--)
    {
        stereoData[i].right = monoData[i];
        stereoData[i].left = monoData[i];
    }

    length = samples * sizeof(StereoData);
    a2dpBytes += length;
    return length;
}


void displayWaveInfo()
{
    Tracer tracer(F(__func__));

    float* octavePower = nullptr;
    uint8_t octaves = 0;
    BinInfo fundamental;
    if (WaveBuffer.getNumSamples() >= DSP_FRAME_SIZE)
    {
        WaveBuffer.getSamples(dspBuffer, DSP_FRAME_SIZE);
        complex_t* complexSpectrum = DSP.runFFT(dspBuffer);
        float* spectralPower = DSP.getSpectralPower(complexSpectrum);
        octavePower = DSP.getOctavePower(spectralPower);
        octaves = DSP.getOctaves();
        fundamental = DSP.getFundamental(spectralPower);
    }

    WaveStats waveStats = WaveBuffer.getStatistics(SAMPLE_FREQUENCY / 2); // last 0.5s
    float dBFS = 20 * log10f(float(waveStats.peak) / FULL_SCALE);

    Display.clearBuffer();
    drawVUMeter(dBFS, octavePower, octaves, displayWidth, displayHeight - 22);
    Display.setFont(u8g2_font_9x15_tf);
    Display.setCursor(0, displayHeight - 1);
    Display.printf(
        "%0.0f dB  %0.0f Hz  %d %%",
        dBFS,
        fundamental.getCenterFrequency(),
        WaveBuffer.getFillPercentage()
        );
    Display.sendBuffer();
}


void drawVUMeter(float vu, float* octavePower, uint8_t octaves, uint8_t width, uint8_t height)
{
    int vuBarSegments = width / 4;
    int vuBarWidth = 8;
    int vuSegments = vuBarSegments * (vu + DB_MIN) / DB_MIN;
    for (int s = 0; s < vuSegments; s++)
    {
        Display.drawBox(s * 4, 0, 3, vuBarWidth - 2);
    }
    for (int s = vuSegments; s < vuBarSegments; s++)
    {
        Display.drawPixel(s * 4 + 1, (vuBarWidth / 2 - 1));
    }

    // Octave bars
    if (octaves > 0)
    {
        Display.drawFrame(0, vuBarWidth, width, height - vuBarWidth);

        int octaveBarWidth = (width - 2) / octaves;
        int octaveBarSegments = (height - vuBarWidth - 2) / 3;
        for (int o = 0; o < octaves; o++)
        {
            int barX = 1 + o * octaveBarWidth;

            float dBoctave = 10 * log10f(octavePower[o]);
            int dBoctaveSegments = octaveBarSegments * (dBoctave + DB_MIN) / DB_MIN;
            for (int s = 0; s < dBoctaveSegments; s++)
            {
                int segmentY = height - 3 - (s * 3);
                Display.drawBox(barX, segmentY, octaveBarWidth - 1, 2);
            }
            for (int s = dBoctaveSegments; s < octaveBarSegments; s++)
            {
                int segmentY = height - 4 - (s * 3);
                Display.drawPixel(barX + octaveBarWidth / 2, segmentY);
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
    HttpResponse.printf(F("<tr><td>A2DP bytes</td><td>%u</td></tr>\r\n"), a2dpBytes);
    HttpResponse.printf(F("<tr><td><a href=\"/events\">Events logged</a></td><td>%d</td></tr>\r\n"), EventLog.count());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<h1>Bluetooth Status</h1>"));
    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>State</td><td>%s</td></tr>\r\n"), BTAudio.getStateName().c_str());
    HttpResponse.printf(F("<tr><td>Remote device</td><td>%s</td></tr>\r\n"), BTAudio.getRemoteDevice().c_str());
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%d</td></tr>\r\n"), BTAudio.getSampleRate());
    HttpResponse.println(F("</table>"));

    HttpResponse.println(F("<p><a href=\"/bt\">Bluetooth</a></p>"));
    
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

    Html.writeHeader(F("Bluetooth"), true, true, 5);

    HttpResponse.println(F("<table class=\"status\">"));
    HttpResponse.printf(F("<tr><td>State</td><td>%s</td></tr>\r\n"), BTAudio.getStateName().c_str());
    HttpResponse.printf(F("<tr><td>Remote device</td><td>%s</td></tr>\r\n"), BTAudio.getRemoteDevice().c_str());
    HttpResponse.printf(F("<tr><td>Sample rate</td><td>%d</td></tr>\r\n"), BTAudio.getSampleRate());
    HttpResponse.printf(F("<tr><td>A2DP Bytes</td><td>%u</td></tr>\r\n"), a2dpBytes);
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

    if (shouldPerformAction(F("disconnect")))
    {
        if (BTAudio.disconnectSource())
        {
            HttpResponse.println(F("<p>Disconnecting...</p>\r\n"));
        }
        else
            HttpResponse.println(F("<p>Disconnect failed.</p>\r\n"));
    }
    else if (BTAudio.isSourceStarted())
        HttpResponse.printf(F("<p><a href=\"?disconnect=%u\">Disconnect</a></p>\r\n"), currentTime);

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

    // Generate square wave signal @ Fs/64 with small DC offset
    WaveBuffer.clear();
    for (int i = 0; i < SAMPLE_FREQUENCY; i++)
    {
        int16_t squarewave = ((i % 64) < 32) ? 32000 : -32000;
        squarewave += 700;
        WaveBuffer.addSample(squarewave);
    }
}

void handleHttpWaveRequest()
{
    Tracer tracer(F(__func__));

    if (shouldPerformAction(F("clear")))
        WaveBuffer.clear();

    if (shouldPerformAction(F("startMic")))
    {
        WaveBuffer.clear();
        recordMic = true;
        displayWaveInfoMillis = millis();
    }

    if (shouldPerformAction(F("stopMic")))
        recordMic = false;

    if (shouldPerformAction(F("test")))
        testFillWaveBuffer();

    bool isRecording = recordMic || BTAudio.isSinkStarted();
    uint16_t refreshInterval = isRecording ? 2 : 0;

    Html.writeHeader(F("Wave Buffer"), true, true, refreshInterval);

    if (WaveBuffer.getNumSamples() != 0)
        HttpResponse.printf(F("<p><a href=\"?clear=%u\">Clear buffer</a></p>\r\n"), currentTime);

    if (recordMic)
        HttpResponse.printf(F("<p><a href=\"?stopMic=%u\">Stop microphone</a></p>\r\n"), currentTime);

    if (!isRecording)
    {
        HttpResponse.printf(F("<p><a href=\"?startMic=%u\">Start microphone</a></p>\r\n"), currentTime);
        HttpResponse.printf(F("<p><a href=\"?test=%u\">Test fill wih squarewave</a></p>\r\n"), currentTime);
        HttpResponse.println(F("<p><a href=\"/wave/dsp\">DSP</a></p>"));
        if (isFTPEnabled)
            HttpResponse.println(F("<p><a href=\"/wave/ftp\">Write to FTP Server</a></p>"));
    }

    WaveStats waveStats = WaveBuffer.getStatistics(); // Get stats for whole buffer

    HttpResponse.println(F("<table class=\"waveStats\">"));
    HttpResponse.printf(
        F("<tr><th>Samples</th><td>%u (%d %%)</td></tr>\r\n"),
        WaveBuffer.getNumSamples(),
        WaveBuffer.getFillPercentage()
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
