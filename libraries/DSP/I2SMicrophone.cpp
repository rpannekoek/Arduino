#include <Arduino.h>
#include <Tracer.h>
#include "I2SMicrophone.h"

// According to TRM: M >= 2
#define M 2
#define CHANNELS 2
#define DMA_BUFFER_SAMPLES 512

// Constructor
I2SMicrophone::I2SMicrophone(ISampleBuffer& sampleBuffer, int sampleRate, i2s_port_t i2sPort, int bckPin, int wsPin, int dataPin)
    : _sampleBuffer(sampleBuffer)
{
    _i2sPort = i2sPort;
    _i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL3, // interrupt priority
        .dma_buf_count = 2,
        .dma_buf_len = DMA_BUFFER_SAMPLES,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        // Using fixed MCLK, so I2SDAC (16 bits) and I2SMicrophone (32 bits) can both use APLL
        .fixed_mclk = M * CHANNELS * I2S_BITS_PER_SAMPLE_32BIT * sampleRate   
        };
    _i2sPinConfig = {
        .bck_io_num = bckPin,
        .ws_io_num = wsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = dataPin,
    };
    _transferBuffer = new int32_t[DMA_BUFFER_SAMPLES];
}


bool I2SMicrophone::begin()
{
    Tracer tracer(F("I2SMicrophone::begin"));

    esp_err_t err = i2s_driver_install(_i2sPort, &_i2sConfig, 0, nullptr);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_driver_install returned %X\n"), err);
        return false;        
    }

    err = i2s_set_pin(_i2sPort, &_i2sPinConfig);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_set_pin returned %X\n"), err);
        return false;        
    }

    err = i2s_start(_i2sPort);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_start returned %X\n"), err);
        return false;        
    }

    xTaskCreatePinnedToCore(
        dataSinkTask,
        "Mic Data Sink",
        4096, // Stack Size (words)
        this, // taskParams
        configMAX_PRIORITIES - 1, // Priority
        &_dataSinkTaskHandle,
        PRO_CPU_NUM // Core ID
        );

    // Give Data Sink Task some time to spin up
    delay(100);

    return _dataSinkTaskHandle != nullptr;
}


bool I2SMicrophone::startRecording()
{
    Tracer tracer(F("I2SMicrophone::startRecording"));

    if (_isRecording)
    {
        TRACE(F("Recording was started already\n"));
        return false;
    }

    _isRecording = true;
    return true;
}


bool I2SMicrophone::stopRecording()
{
    Tracer tracer(F("I2SMicrophone::stopRecording"));

    if (!_isRecording)
    {
        TRACE(F("Recording is not running\n"));
        return false;
    }

    _isRecording = false;
    return true;
}


bool I2SMicrophone::setGain(float dB)
{
    _scale = roundf(65536.0 / pow10f(dB / 20));
}


float I2SMicrophone::getGain()
{
    return 20 * log10f(65536 / _scale);
}


float I2SMicrophone::adjustGain(float dBFS)
{
    int32_t newScale = 0;
    if ((dBFS >= -3) && (_scale <= 32768)) newScale = _scale * 2;
    if ((dBFS <= -6) && (_scale >= 342)) newScale = (_scale * 3) / 4;
    if (newScale != 0)
    {
        TRACE(F("AGC: %0.0f db => %d -> %d\n"), dBFS, _scale, newScale);
        _scale = newScale;
    }

    return getGain();
}


void I2SMicrophone::dataSink()
{
    Tracer tracer(F("I2SMicrophone::dataSink"));

    TickType_t msTimeout = 2 * 1000 * _i2sConfig.dma_buf_len / _i2sConfig.sample_rate;
    size_t bytesToRead = DMA_BUFFER_SAMPLES * sizeof(int32_t);

    while (true)
    {
        size_t bytesRead;
        esp_err_t err = i2s_read(_i2sPort, _transferBuffer, bytesToRead, &bytesRead, msTimeout);
        if (err != ESP_OK)
        {
            TRACE(F("i2s_read returned %X\n"), err);
            continue; // Stopping a task is not allowed
        }
        if (bytesRead < bytesToRead)
        {
            TRACE(F("i2s_read timeout\n"));
            continue; // Stopping a task is not allowed
        }

        if (!_isRecording) continue;

        int32_t scale = _scale;
        for (int i = 0; i < DMA_BUFFER_SAMPLES; i++)
        {
            _transferBuffer[i] /= scale;
        }

        _sampleBuffer.addSamples(_transferBuffer, DMA_BUFFER_SAMPLES);
    }
}


void I2SMicrophone::dataSinkTask(void* taskParams)
{
    I2SMicrophone* instancePtr = (I2SMicrophone*)taskParams;
    instancePtr->dataSink();
}
