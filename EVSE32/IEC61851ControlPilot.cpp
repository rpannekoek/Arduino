#include <Tracer.h>
#include "IEC61851ControlPilot.h"

#define PWM_FREQ 1000
#define STATUS_POLL_INTERVAL 1.0F
#define OVERSAMPLING 5
#define ADC_OFFSET 0.7F

const char* _statusNames[] =
{
    "Standby",
    "Vehicle detected",
    "Charging",
    "Charging (ventilated)",
    "No power"
};


const char* IEC61851ControlPilot::getStatusName()
{
    return _statusNames[static_cast<int>(_status)];
}


IEC61851ControlPilot::IEC61851ControlPilot(uint8_t outputPin, uint8_t inputPin, uint8_t feedbackPin,uint8_t pwmChannel, float maxCurrent)
{
    _outputPin = outputPin;
    _inputPin = inputPin;
    _feedbackPin = feedbackPin;
    _pwmChannel = pwmChannel;
    _maxCurrent = maxCurrent;
}


bool IEC61851ControlPilot::begin(float scale)
{
    Tracer tracer(F("IEC61851ControlPilot::begin"));

    _dutyCycle = 0;
    _scale = scale; 

    int8_t adcChannel = digitalPinToAnalogChannel(_inputPin);
    if (adcChannel < 0 || adcChannel >= ADC1_CHANNEL_MAX)
    {
        TRACE(F("Pin %d has no associated ADC1 channel.\n"));
        return false;
    }
    else
        TRACE(F("Pin %d => ADC1 channel %d\n"), _inputPin, adcChannel);
    _adcChannel = static_cast<adc1_channel_t>(adcChannel);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(_adcChannel, ADC_ATTEN_DB_11);

    pinMode(_inputPin, ANALOG);
    pinMode(_outputPin, OUTPUT);
    pinMode(_feedbackPin, INPUT);

    digitalWrite(_outputPin, 0); // 0 V

    _statusTicker.attach(STATUS_POLL_INTERVAL, determineStatus, this);

    uint32_t freq = ledcSetup(_pwmChannel, PWM_FREQ, 8); // 1 kHz, 8 bits
    return freq == PWM_FREQ;
}


float IEC61851ControlPilot::calibrate()
{
    Tracer tracer(F("IEC61851ControlPilot::calibrate"));

    if (_dutyCycle == 0)
    {
        digitalWrite(_outputPin, 1); // 12 V
        delay(10);
    }

    int standbyLevel = 0;
    for (int i = 0; i < OVERSAMPLING; i++)
        standbyLevel += adc1_get_raw(_adcChannel);
    standbyLevel /= OVERSAMPLING;

    if (_dutyCycle == 0)
        digitalWrite(_outputPin, 0); // 0 V

    if (standbyLevel > 2500)
    {
        _scale = (12.0F - ADC_OFFSET) / standbyLevel;
        TRACE(F("Standby level: %d => scale = %0.4f\n"), standbyLevel, _scale);
    }
    else
        TRACE(F("Invalid standby level: %d\n"), standbyLevel);

    return _scale;
}


void IEC61851ControlPilot::setOff()
{
    Tracer tracer(F("IEC61851ControlPilot::setOff"));

    ledcDetachPin(_outputPin);
    digitalWrite(_outputPin, 0); // 0 V

    _dutyCycle = 0;
}


void IEC61851ControlPilot::setReady()
{
    Tracer tracer(F("IEC61851ControlPilot::setReady"));

    ledcDetachPin(_outputPin);
    digitalWrite(_outputPin, 1); // 12 V

    _dutyCycle = 1;
}


float IEC61851ControlPilot::setCurrentLimit(float ampere)
{
    Tracer tracer(F("IEC61851ControlPilot::setCurrentLimit"));

    ampere = std::min(std::max(ampere, 6.0F), _maxCurrent);
    _dutyCycle =  ampere / 60.0F;
    uint32_t duty = static_cast<uint32_t>(std::round(_dutyCycle * 256));

    ledcAttachPin(_outputPin, _pwmChannel);
    ledcWrite(_pwmChannel, duty);

    TRACE(
        F("Set current limit %0.1f A. Duty cycle %0.0f %% (%d)\n"),
        ampere,
        _dutyCycle * 100,
        duty);

    return ampere;
}


float IEC61851ControlPilot::getVoltage()
{
    uint32_t originalDuty = 0;
    if (_dutyCycle > 0 && _dutyCycle < 1)
    {
        // Can't measure voltage is duty cycle is very low   
        originalDuty = ledcRead(_pwmChannel);
        if (originalDuty < 32) ledcWrite(_pwmChannel, 32);

        // Wait for CP output low -> high transition
        int i = 0;
        while (digitalRead(_feedbackPin) == 1)
        {
            if (i++ == 150)
            {
                TRACE(F("Timeout waiting for CP low\n"));
                return -1;
            }
            delayMicroseconds(10);
        }
        while (digitalRead(_feedbackPin) == 0)
        {
            if (i++ == 300)
            {
                TRACE(F("Timeout waiting for CP high\n"));
                return -1;
            }
            delayMicroseconds(10);
        }
        delayMicroseconds(5); // Just switched to high; give signal some time to settle
    }

    int sample = adc1_get_raw(_adcChannel);
    float voltage = (sample < 5) ? 0.0F : _scale * sample + ADC_OFFSET;

    if ((_dutyCycle > 0) && (_dutyCycle < 0.125))
        ledcWrite(_pwmChannel, originalDuty);

    return voltage;
}


bool IEC61851ControlPilot::awaitStatus(ControlPilotStatus status, int timeoutMs)
{
    while (_status != status && timeoutMs > 0)
    {
        delay(10);
        timeoutMs -= 10;
        determineStatus();
    }
    return _status == status;
}


void IEC61851ControlPilot::determineStatus() 
{
    float voltage;
    int retries = 3;
    do
    {
        voltage = getVoltage();
        if (voltage == 0.0F && _dutyCycle > 0 && _dutyCycle < 1 && retries > 0)
        {
            TRACE(F("Measured 0 V with duty cycle %0.0f. Retrying...\n"), _dutyCycle * 100);
            voltage = -1;
        }
    }
    while (voltage < 0 && retries-- > 0);

    if (voltage > 10.5)
        _status =  ControlPilotStatus::Standby;
    else if (voltage > 7.5)
        _status = ControlPilotStatus::VehicleDetected;
    else if (voltage > 4.5)
        _status = ControlPilotStatus::Charging;
    else if (voltage > 1.5)
        _status = ControlPilotStatus::ChargingVentilated;
    else
        _status = ControlPilotStatus::NoPower;
}


void IEC61851ControlPilot::determineStatus(IEC61851ControlPilot* instancePtr)
{
    instancePtr->determineStatus();
}
