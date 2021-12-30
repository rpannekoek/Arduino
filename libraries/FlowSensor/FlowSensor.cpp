#include "FlowSensor.h"
#include <Tracer.h>

uint8_t FlowSensor::_pinInterrupt;
float FlowSensor::_measureInterval;
float FlowSensor::_pulseFreq;
uint32_t FlowSensor::_pulseCount;
float FlowSensor::_flowRate;


FlowSensor::FlowSensor(uint8_t pin)
{
    _pinInterrupt = digitalPinToInterrupt(pin);

    pinMode(pin, INPUT);
    digitalWrite(pin, HIGH);    
}


bool FlowSensor::begin(float measureInterval, float pulseFreq)
{
    Tracer Tracer(F("FlowSensor::begin"));

    _measureInterval = measureInterval;
    _pulseFreq = pulseFreq;
    _pulseCount = 0;
    _flowRate = 0;

    TRACE(F("Using interrupt: %d\n"), _pinInterrupt);

    attachInterrupt(_pinInterrupt, pulseISR, FALLING);
    _ticker.attach(measureInterval, measure);
}


void FlowSensor::end()
{
    Tracer Tracer(F("FlowSensor::end"));

    _ticker.detach();
    detachInterrupt(_pinInterrupt);
}


void FlowSensor::pulseISR()
{
    _pulseCount++;
} 


void FlowSensor::measure()
{
    Tracer Tracer(F("FlowSensor::measure"));

    detachInterrupt(_pinInterrupt);
    uint32_t pulseCount = _pulseCount;
    _pulseCount = 0;
    attachInterrupt(_pinInterrupt, pulseISR, FALLING);

    _flowRate = float(pulseCount) / ( _pulseFreq * _measureInterval);
    TRACE(F("Pulse count: %d => Flow rate: %0.1f l/min\n"), pulseCount, _flowRate);
}