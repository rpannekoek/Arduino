#include <Tracer.h>
#include "CurrentSensor.h"

#define SAMPLE_INTERVAL_MS 1
#define PERIOD_MS 20

CurrentSensor::CurrentSensor(int8_t pin)
{
    _pin = pin;
}


bool CurrentSensor::begin(uint16_t zero, float scale)
{
    Tracer tracer(F("CurrentSensor::begin"));

    pinMode(_pin, ANALOG);

    _zero = zero;
    _scale = scale;

    return true;
}


uint16_t CurrentSensor::measure(uint16_t periods)
{
    Tracer tracer(F("CurrentSensor::measure"));

    if (_sampleBufferPtr != nullptr)
        delete[] _sampleBufferPtr;

    _sampleBufferSize = periods * PERIOD_MS / SAMPLE_INTERVAL_MS;
    _sampleBufferPtr = new uint16_t[_sampleBufferSize]; 
    _sampleIndex = 0;

    _ticker.attach_ms(SAMPLE_INTERVAL_MS, sample, this);
    delay(periods * PERIOD_MS);
    _ticker.detach();

    TRACE(F("Sampled %d periods => %d samples.\n"), periods, _sampleIndex);

    return _sampleIndex;
}


uint16_t CurrentSensor::calibrateZero()
{
    Tracer tracer(F("CurrentSensor::calibrateZero"));

    if (_sampleIndex == 0)
    {
        TRACE(F("No samples\n"));
        return 0;
    }

    uint32_t total = 0;
    for (uint16_t i = 0; i < _sampleIndex; i++)
        total += _sampleBufferPtr[i];

    _zero = total / _sampleIndex; // Average

    TRACE(F("Zero set to %d\n"), _zero);

    return _zero;
}


float CurrentSensor::calibrateScale(float actualRMS)
{
    Tracer tracer(F("CurrentSensor::calibrateScale"));

    float measuredRMS = getRMS();
    _scale *= actualRMS / measuredRMS;

    TRACE(F("Measured %0.3f A, Actual %0.3f A => scale = %0.3f"), measuredRMS, actualRMS, _scale);

    return _scale;
}


float CurrentSensor::getPeak()
{
    if (_sampleBufferPtr == nullptr || _sampleIndex == 0)
        return 0.0F;

    float peak = 0;
    for (int i = 0; i < _sampleIndex; i++)
        peak = std::max(peak, std::abs(getSample(i)));

    return peak;
}


float CurrentSensor::getRMS()
{
    if (_sampleBufferPtr == nullptr || _sampleIndex == 0)
        return 0.0F;

    float sumSquares = 0;
    for (int i = 0; i < _sampleIndex; i++)
        sumSquares += pow(getSample(i), 2);

    return sqrt(sumSquares / _sampleIndex);
}


float CurrentSensor::getDC()
{
    if (_sampleBufferPtr == nullptr || _sampleIndex == 0)
        return 0.0F;

    float total = 0;
    for (uint16_t i = 0; i < _sampleIndex; i++)
        total += getSample(i);

    return total / _sampleIndex; // Average
}


void CurrentSensor::sample(CurrentSensor* instancePtr)
{
    uint16_t sampleIndex = instancePtr->_sampleIndex++;
    instancePtr->_sampleBufferPtr[sampleIndex] = analogRead(instancePtr->_pin);
}
