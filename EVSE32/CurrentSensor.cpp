#include <driver/adc.h>
#include <Tracer.h>
#include "CurrentSensor.h"

#define SAMPLE_INTERVAL_MS 1
#define OVERSAMPLING 5
#define PERIOD_MS 20

CurrentSensor::CurrentSensor(uint8_t pin, size_t bufferSize)
{
    _pin = pin;
    _sampleBufferSize = bufferSize;
    _sampleBufferPtr = new uint16_t[bufferSize];
}


bool CurrentSensor::begin(uint16_t zero, float scale)
{
    Tracer tracer(F("CurrentSensor::begin"));

    int8_t adcChannel = digitalPinToAnalogChannel(_pin);
    if (adcChannel < 0 || adcChannel >= ADC1_CHANNEL_MAX)
    {
        TRACE(F("Pin %d has no associated ADC1 channel.\n"));
        return false;
    }
    else
        TRACE(F("Pin %d => ADC1 channel %d\n"), _pin, adcChannel);
    _adcChannel = static_cast<adc1_channel_t>(adcChannel);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(_adcChannel, ADC_ATTEN_DB_11);

    pinMode(_pin, ANALOG);

    _zero = zero;
    _scale = scale;

    return true;
}


void CurrentSensor::measure(uint16_t periods)
{
    Tracer tracer(F("CurrentSensor::measure"));

    uint16_t maxPeriods = _sampleBufferSize * SAMPLE_INTERVAL_MS / PERIOD_MS;
    periods = std::min(periods, maxPeriods);
    TRACE(F("Measuring %d periods...\n"), periods);

    _sampleIndex = 0;

    _ticker.attach_ms(SAMPLE_INTERVAL_MS, sample, this);
    delay(periods * PERIOD_MS);
    _ticker.detach();
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
    if ((measuredRMS > 0) && (measuredRMS < 100))
    {
        _scale *= actualRMS / measuredRMS;
        TRACE(F("Measured %0.3f A, Actual %0.3f A => scale = %0.3f\n"), measuredRMS, actualRMS, _scale);
    }
    else
    {
        _scale = 0.016;
        TRACE(F("Measured RMS out of range. Reset scale.\n"));
    }
 
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


void  CurrentSensor::writeSampleCsv(Print& writeTo, bool raw)
{
    String csvHeader = raw ? F("DC, AC") : F("I (A)");
    writeTo.println(csvHeader);

    for (uint16_t i = 0; i < _sampleIndex; i++)
    {
        if (raw)
            writeTo.printf("%d, %d\n", _sampleBufferPtr[i], (int)_sampleBufferPtr[i] - _zero);
        else
            writeTo.printf("%0.3f\n", getSample(i));
    }
}


void CurrentSensor::sample(CurrentSensor* instancePtr)
{
    int sample = 0;
    for (int i = 0; i < OVERSAMPLING; i++)
        sample += adc1_get_raw(instancePtr->_adcChannel);
    sample /= OVERSAMPLING;

    uint16_t sampleIndex = instancePtr->_sampleIndex++;
    if (sampleIndex < instancePtr->_sampleBufferSize)
        instancePtr->_sampleBufferPtr[sampleIndex] = std::max(sample, 0);
}
