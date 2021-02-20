#include <Arduino.h>
#include <Tracer.h>
#include "I2SDAC.h"

// According to TRM: M >= 2 (see comment about fixed_mclk below)
#define M 4
#define CHANNELS 2
#define DMA_BUFFER_SAMPLES 512


// Constructor for internal DAC
I2SDAC::I2SDAC(WaveBuffer& waveBuffer, int sampleRate, i2s_port_t i2sPort, int timingPin) 
    : _waveBuffer(waveBuffer)
{
    _i2sPort = i2sPort;
    _i2sConfig = 
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = i2s_comm_format_t::I2S_COMM_FORMAT_PCM,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2, // interrupt priority
        .dma_buf_count = 2,
        .dma_buf_len = DMA_BUFFER_SAMPLES,
        .use_apll = false, // TODO: supported for internal DAC?
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0   
    };
    _i2sPinConfig = nullptr;
    _timingPin = timingPin;
}

// Constructor for external DAC
I2SDAC::I2SDAC(WaveBuffer& waveBuffer, int sampleRate, i2s_port_t i2sPort, int bckPin, int wsPin, int dataPin, int timingPin)
    : _waveBuffer(waveBuffer)
{
    _i2sPort = i2sPort;
    _i2sConfig = 
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = i2s_comm_format_t::I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2, // interrupt priority
        .dma_buf_count = 2,
        .dma_buf_len = DMA_BUFFER_SAMPLES,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        // Using fixed MCLK, so I2SDAC (16 bits) and I2SMicrophone (32 bits) can both use APLL
        .fixed_mclk = M * CHANNELS * I2S_BITS_PER_SAMPLE_16BIT * sampleRate   
    };
    _i2sPinConfig = new i2s_pin_config_t {
        .bck_io_num = bckPin,
        .ws_io_num = wsPin,
        .data_out_num = dataPin,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    _timingPin = timingPin;
}


bool I2SDAC::begin()
{
    Tracer tracer(F("I2SDAC::begin"));

    esp_err_t err = i2s_driver_install(_i2sPort, &_i2sConfig, 0, nullptr);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_driver_install returned %X\n"), err);
        return false;        
    }

    if (_i2sPinConfig == nullptr)
    {
        // Internal DAC
        err = i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);
        if (err != ESP_OK)
        {
            TRACE(F("i2s_set_dac_mode returned %X\n"), err);
            return false;
        }
    }
    else
    {
        // External DAC
        err = i2s_set_pin(_i2sPort, _i2sPinConfig);
        if (err != ESP_OK)
        {
            TRACE(F("i2s_set_pin returned %X\n"), err);
            return false;        
        }
    }

    err = i2s_start(_i2sPort);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_start returned %X\n"), err);
        return false;        
    }

    _sampleBuffer = (int16_t*) malloc(DMA_BUFFER_SAMPLES * sizeof(int16_t));
    if (_sampleBuffer == nullptr)
    {
        TRACE(F("Unable to allocate sample buffer\n"));
        return false;
    }

    xTaskCreatePinnedToCore(
        dataSourceTask,
        "DAC Data Source",
        4096, // Stack Size (words)
        this, // taskParams
        configMAX_PRIORITIES - 1, // Priority
        &_dataSourceTaskHandle,
        PRO_CPU_NUM // Core ID
        );

    // Give Data Sink Task some time to spin up
    delay(100);

    return _dataSourceTaskHandle != nullptr;
}


bool I2SDAC::startPlaying()
{
    Tracer tracer(F("I2SDAC::startPlaying"));

    if (_isPlaying)
    {
        TRACE(F("Already playing\n"));
        return false;
    }

    _isPlaying = true;
    return true;
}


bool I2SDAC::stopPlaying()
{
    Tracer tracer(F("I2SDAC::stopPlaying"));

    if (!_isPlaying)
    {
        TRACE(F("Not currently playing\n"));
        return false;
    }

    _isPlaying = false;
    return true;
}


void I2SDAC::dataSource()
{
    Tracer tracer(F("I2SDAC::dataSource"));

    size_t bytesToWrite = DMA_BUFFER_SAMPLES * sizeof(int16_t);
    TickType_t msTimeout = 2 * 1000 * _i2sConfig.dma_buf_len / _i2sConfig.sample_rate;

    while (true)
    {
        if (!_isPlaying) 
        {
            vTaskDelay(100);
            continue;
        }

        if (_timingPin >= 0) digitalWrite(_timingPin, 1);

        _waveBuffer.getNewSamples(_sampleBuffer, DMA_BUFFER_SAMPLES);

        if (_timingPin >= 0) digitalWrite(_timingPin, 0);

        size_t bytesWritten = 0;
        esp_err_t err = i2s_write(_i2sPort, _sampleBuffer, bytesToWrite, &bytesWritten, msTimeout);
        if (err != ESP_OK)
        {
            TRACE(F("i2s_write returned %X\n"), err);
            continue; // Stopping a task is not allowed
        }
        if (bytesWritten < bytesToWrite)
        {
            TRACE(F("i2s_write timeout\n"));
            continue; // Stopping a task is not allowed
        }
    }
}


void I2SDAC::dataSourceTask(void* taskParams)
{
    I2SDAC* instancePtr = (I2SDAC*)taskParams;
    instancePtr->dataSource();
}
