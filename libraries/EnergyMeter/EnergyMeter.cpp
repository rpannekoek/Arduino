#include "EnergyMeter.h"
#include <Tracer.h>

uint8_t EnergyMeter::_pinInterrupt;
uint16_t EnergyMeter::_pulsesPerKWh;
float EnergyMeter::_measureInterval;
uint32_t EnergyMeter::_lastPulseMillis;
uint32_t EnergyMeter::_pulseCount;
uint32_t EnergyMeter::_energyPulseCount;
float EnergyMeter::_power;


EnergyMeter::EnergyMeter(uint8_t pin)
{
    _pinInterrupt = digitalPinToInterrupt(pin);

    pinMode(pin, INPUT);
    digitalWrite(pin, HIGH);    
}


bool EnergyMeter::begin(uint16_t resolutionWatt, uint16_t pulsesPerKWh)
{
    Tracer Tracer(F("EnergyMeter::begin"));

    _pulsesPerKWh = pulsesPerKWh;
    _measureInterval =  3600000.0 / float(resolutionWatt * pulsesPerKWh);
    _lastPulseMillis = 0;
    _energyPulseCount = 0;
    _pulseCount = 0;
    _power = 0;

    TRACE(F("Resolution: %d W => Interval: %0.1f s\n"), resolutionWatt, _measureInterval);
    TRACE(F("Using interrupt: %d\n"), _pinInterrupt);

    attachInterrupt(_pinInterrupt, pulseISR, FALLING);
    _ticker.attach(_measureInterval, measure);
}


void EnergyMeter::end()
{
    Tracer Tracer(F("EnergyMeter::end"));

    _ticker.detach();
    detachInterrupt(_pinInterrupt);
}


void EnergyMeter::pulseISR()
{
    // Pulses less than 500 ms apart are ignored (software debounce)
    // 500 ms apart => 7.2 kW
    uint32_t currentMillis = millis();
    if ((currentMillis - _lastPulseMillis) >= 500)
    {
        _pulseCount++;
        _energyPulseCount++;
    }
    _lastPulseMillis = currentMillis;
} 


void EnergyMeter::measure()
{
    Tracer Tracer(F("EnergyMeter::measure"));

    detachInterrupt(_pinInterrupt);
    uint32_t pulseCount = _pulseCount;
    _pulseCount = 0;
    attachInterrupt(_pinInterrupt, pulseISR, FALLING);

    _power = 3600000.0 * float(pulseCount) / ( _measureInterval * _pulsesPerKWh);
    TRACE(F("Pulse count: %d => Power: %0.1f W\n"), pulseCount, _power);
}