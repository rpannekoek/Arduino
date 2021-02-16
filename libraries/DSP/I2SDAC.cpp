#include <Arduino.h>
#include <Tracer.h>
#include "I2SDAC.h"


i2s_config_t i2sDacConfig = 
{
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = i2s_comm_format_t::I2S_COMM_FORMAT_PCM,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2, // interrupt priority
    .dma_buf_count = 2,
    .dma_buf_len = DAC_BUFFER_SAMPLES, // samples
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0   
};


bool I2SDAC::begin(i2s_port_t i2sPort, int sampleRate)
{
    Tracer tracer(F("I2SDAC::begin"));

    _i2sPort = i2sPort;
    i2sDacConfig.sample_rate = sampleRate;

    esp_err_t err = i2s_driver_install(i2sPort, &i2sDacConfig, 0, nullptr);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_driver_install returned %X\n"), err);
        return false;        
    }

    err = i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_set_dac_mode returned %X\n"), err);
        return false;        
    }

    /*
    err = i2s_start(i2sPort);
    if (err != ESP_OK)
    {
        TRACE(F("i2s_start returned %X\n"), err);
        return false;        
    }
    */

    _sampleBuffer = (int16_t*) ps_malloc(DAC_BUFFER_SAMPLES * sizeof(int16_t));
    if (_sampleBuffer == nullptr)
    {
        TRACE(F("Uanble to allocate sample buffer\n"));
        return false;
    }

    xTaskCreatePinnedToCore(
        dataSourceTask,
        "DAC Data Source",
        8192, // Stack Size (words)
        this, // taskParams
        3, // Priority
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
        TRACE(F("Playing was started already\n"));
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
        TRACE(F("Playing not running\n"));
        return false;
    }

    _isPlaying = false;
    return true;
}


void I2SDAC::dataSource()
{
    Tracer tracer(F("I2SDAC::dataSource"));

    while (true)
    {
        if (!_isPlaying) 
        {
            vTaskDelay(10);
            continue;
        }

        // TODO: wait (?)
        _waveBuffer.getNewSamples(_sampleBuffer, DAC_BUFFER_SAMPLES, 0);

        size_t bytesToWrite = DAC_BUFFER_SAMPLES * sizeof(int16_t);
        size_t bytesWritten = 0;
        esp_err_t err = i2s_write(_i2sPort, _sampleBuffer, bytesToWrite, &bytesWritten, 15 /*ms*/ );
        if (err != ESP_OK)
        {
            TRACE(F("i2s_write returned %X\n"), err);
            continue; // Stopping a task is not allowed
        }
        if (bytesWritten < bytesToWrite)
        {
            TRACE(F("I2S DAC timeout\n"));
            continue; // Stopping a task is not allowed
        }
    }
}


void I2SDAC::dataSourceTask(void* taskParams)
{
    I2SDAC* instancePtr = (I2SDAC*)taskParams;
    instancePtr->dataSource();
}
