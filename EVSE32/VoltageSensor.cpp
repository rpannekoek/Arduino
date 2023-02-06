#include <Tracer.h>
#include "VoltageSensor.h"


VoltageSensor::VoltageSensor(uint8_t pin)
{
    _pin = pin;
    _pinInterrupt = digitalPinToInterrupt(pin);
}


bool VoltageSensor::begin()
{
    Tracer tracer(F("VoltageSensor::begin"));

    attachInterruptArg(_pinInterrupt, edgeISR, this, RISING);
    return true;
}


bool VoltageSensor::detectSignal(uint32_t sensePeriodMs)
{
    Tracer tracer(F("VoltageSensor::detectSignal"));

    _edgeCount = 0;
    delay(sensePeriodMs);

    TRACE(F("Detected %d edges in %d ms\n"), _edgeCount, sensePeriodMs);

    return (_edgeCount > 0);
}


void VoltageSensor::edgeISR(void* arg)
{
    VoltageSensor* instancePtr = static_cast<VoltageSensor*>(arg);
    instancePtr->_edgeCount++;
}
