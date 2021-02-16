#include <Arduino.h>
#include <Tracer.h>
#include <driver/dac.h>
#include "TimerDAC.h"

TaskHandle_t TimerDAC::_dataSourceTaskHandle = nullptr;


bool TimerDAC::begin(dac_channel_t dacChannel, uint16_t sampleRate)
{
    Tracer tracer(F("TimerDAC::begin"));

    _dacChannel = dacChannel;
    dac_output_enable(dacChannel);
    dac_output_voltage(dacChannel, 128);

    xTaskCreatePinnedToCore(
        dataSourceTask,
        "DAC Data Source",
        4096, // Stack Size (words)
        this, // taskParams
        configMAX_PRIORITIES - 1, // Priority
        &_dataSourceTaskHandle,
        PRO_CPU_NUM // Core ID
        );
    
    if (_dataSourceTaskHandle == nullptr)
    {
        TRACE(F("xTaskCreatePinnedToCore failed.\n"));
        return false;
    }

    // Give Data Sink Task some time to spin up
    delay(100);

    uint32_t timerValue = 80000000UL  / sampleRate;
    TRACE(F("Timer value: %u\n"), timerValue);

    _timer = timerBegin(0, 1, true);
    timerAttachInterrupt(_timer, timerISR, true);
    timerAlarmWrite(_timer, timerValue, true);

    return true;
}


bool TimerDAC::startPlaying()
{
    Tracer tracer(F("TimerDAC::startPlaying"));

    if (_isPlaying)
    {
        TRACE(F("Already playing\n"));
        return false;
    }

    timerAlarmEnable(_timer);
    _isPlaying = true;
    return true;
}


bool TimerDAC::stopPlaying()
{
    Tracer tracer(F("TimerDAC::stopPlaying"));

    if (!_isPlaying)
    {
        TRACE(F("Not currently playing\n"));
        return false;
    }

    timerAlarmDisable(_timer);
    _isPlaying = false;
    return true;
}


void TimerDAC::dataSource()
{
    Tracer tracer(F("TimerDAC::dataSource"));

    while (true)
    {
        xTaskNotifyWait(0, 0, nullptr, portMAX_DELAY);

        int16_t sample = _waveBuffer.getNewSample();

        // Convert 16 bits signed to 8 bits unsigned
        sample = (sample / 256) + 128;

        dac_output_voltage(_dacChannel, sample);
    }
}


void TimerDAC::dataSourceTask(void* taskParams)
{
    TimerDAC* instancePtr = (TimerDAC*)taskParams;
    instancePtr->dataSource();
}


void TimerDAC::timerISR()
{
    BaseType_t higherPrioTaskAwoken = pdFALSE;
    xTaskNotifyFromISR(_dataSourceTaskHandle, 0, eNotifyAction::eNoAction, &higherPrioTaskAwoken);
    if (higherPrioTaskAwoken == pdTRUE)
        portYIELD_FROM_ISR();
}
