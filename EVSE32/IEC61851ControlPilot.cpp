#include <Tracer.h>
#include "IEC61851ControlPilot.h"

#define STATUS_POLL_INTERVAL 1.0F


const char* _statusNames[] =
{
    "Standby",
    "Vehicle Detected",
    "Charging",
    "Charging (Ventilated)",
    "No Power"
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

    pinMode(_inputPin, ANALOG);
    pinMode(_outputPin, OUTPUT);
    pinMode(_feedbackPin, INPUT);

    digitalWrite(_outputPin, 0); // 0 V

    _statusTicker.attach(1.0F, determineStatus, this);

    uint32_t freq = ledcSetup(_pwmChannel, 1000, 8); // 1 kHz, 8 bits
    return freq != 0;
}


float IEC61851ControlPilot::calibrate()
{
    Tracer tracer(F("IEC61851ControlPilot::calibrate"));

    // Assuming CP voltage is 12 V (Standby)
    uint16_t standbyLevel = analogRead(_inputPin);
    _scale = 12.0F / standbyLevel;

    TRACE(F("Standby level: %d => scale = %0.4f\n"), standbyLevel, _scale);

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

    _dutyCycle = 0;
}


float IEC61851ControlPilot::setCurrentLimit(float ampere)
{
    Tracer tracer(F("IEC61851ControlPilot::setCurrentLimit"));

    ampere = std::min(std::max(ampere, 6.0F), _maxCurrent);
    _dutyCycle =  ampere / 60.0F;
    uint32_t duty = static_cast<uint32_t>(std::round(_dutyCycle * 256));

    ledcWrite(_pwmChannel, duty);
    ledcAttachPin(_outputPin, _pwmChannel);

    TRACE(
        F("Set current limit %0.1f. Duty cycle %0.0f %% (%d)"),
        ampere,
        _dutyCycle * 100,
        duty);

    return ampere;
}


float IEC61851ControlPilot::getVoltage()
{
    if (_dutyCycle > 0)
    {
        // Wait till CP output is high
        int i = 0;
        while (digitalRead(_feedbackPin) == 0)
        {
            if (i++ == 200)
            {
                TRACE(F("Timeout waiting for CP high\n"));
                return -1;
            }
            delayMicroseconds(10);
        }
        if (i > 0)
            delayMicroseconds(50); // Just switched to high; give signal some time to settle
    }

    uint16_t analogInput = analogRead(_inputPin);
    float voltage = _scale * analogInput;

    //TRACE(F("Control Pilot %0.1f V (%d * %0.4f)\n"), voltage, analogInput, _scale);

    return voltage;
}


void IEC61851ControlPilot::determineStatus(IEC61851ControlPilot* instancePtr) 
{
    float voltage = instancePtr->getVoltage();

    ControlPilotStatus status;
    if (voltage > 10.5)
        status =  ControlPilotStatus::Standby;
    else if (voltage > 7.5)
        status = ControlPilotStatus::VehicleDetected;
    else if (voltage > 4.5)
        status = ControlPilotStatus::Charging;
    else if (voltage > 1.5)
        status = ControlPilotStatus::ChargingVentilated;
    else
        status = ControlPilotStatus::NoPower;

    instancePtr->_status = status;
}
