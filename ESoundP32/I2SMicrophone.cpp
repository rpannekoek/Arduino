#include <Arduino.h>
#include <Tracer.h>
#include "I2SMicrophone.h"


i2s_config_t i2sConfig = 
{
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL3, // interrupt priority
    .dma_buf_count = 3,
    .dma_buf_len = 512, // samples
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0   
};


bool I2SMicrophone::begin(i2s_port_t i2sPort, int sampleRate, int bckPin, int wsPin, int dataPin)
{
    Tracer tracer(F("I2SMicrophone::begin"));

    _i2sPort = i2sPort;
    i2sConfig.sample_rate = sampleRate;

    esp_err_t err = i2s_driver_install(i2sPort, &i2sConfig, 0, nullptr);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_driver_install returned %X\n"), err);
        return false;        
    }

    i2s_pin_config_t pinConfig =
    {
        .bck_io_num = bckPin,
        .ws_io_num = wsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = dataPin
    };

    err = i2s_set_pin(i2sPort, &pinConfig);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_set_pin returned %X\n"), err);
        return false;        
    }

    err = i2s_start(i2sPort);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_start returned %X\n"), err);
        return false;        
    }

    xTaskCreatePinnedToCore(
        dataSinkTask,
        "Mic Data Sink",
        8192, // Stack Size (words)
        this, // taskParams
        3, // Priority
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

    _recordedSamples = 0;
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

    while (true)
    {
        int32_t micSample;
        size_t bytesRead;
        esp_err_t err = i2s_read(_i2sPort, &micSample, sizeof(int32_t), &bytesRead, 15 /*ms*/);

        uint32_t startCycles = ESP.getCycleCount();
        if (err != ESP_OK)
        {
            TRACE(F("i2s_read returned %X\n"), err);
            continue; // Stopping a task is not allowed
        }
        if (bytesRead < sizeof(int32_t))
        {
            TRACE(F("I2S microphone timeout\n"));
            continue; // Stopping a task is not allowed
        }

        if (!_isRecording) continue;

        _recordedSamples++;

        _fxEngine.addSample(micSample / _scale);

        _cycles = ESP.getCycleCount() - startCycles;
    }
}


void I2SMicrophone::dataSinkTask(void* taskParams)
{
    I2SMicrophone* instancePtr = (I2SMicrophone*)taskParams;
    instancePtr->dataSink();
}
