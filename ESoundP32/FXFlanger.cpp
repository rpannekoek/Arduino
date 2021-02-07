#include <Arduino.h>
#include <Tracer.h>
#include "FXFlanger.h"

#define SINE_SAMPLES 1024

#define CFG_DELAY F("Flanger_Delay")
#define CFG_ATTENUATION F("Flanger_Att")
#define CFG_MOD_FREQ F("Flanger_ModFreq")
#define CFG_MOD_DEPTH F("Flanger_ModDepth")

// Constructor
FXFlanger::FXFlanger(uint16_t sampleRate)
{
    _sampleRate = sampleRate;
    _delay = sampleRate / 10; // 100 ms
    _attenuation = 40;
    _modulationPeriod = sampleRate; // 1 s
    _modulationDepth = _delay / 2;
    _sineTable = (float*) ps_malloc(SINE_SAMPLES * sizeof(float));

    for (int i = 0; i < SINE_SAMPLES; i++)
    {
        float phi = 2.0 * PI * i / SINE_SAMPLES;
        _sineTable[i] = sinf(phi);
    }
}


void FXFlanger::writeConfigForm(HtmlWriter& html)
{
    int msDelay = roundf(1000.0 * _delay / _sampleRate);
    int modFreq = _sampleRate / _modulationPeriod;
    int modPercent = 100 * _modulationDepth / _delay;

    html.writeSlider(CFG_DELAY, F("Delay"), F("ms"), msDelay, 2, 200);
    html.writeSlider(CFG_ATTENUATION, F("Attenuation"), F("/8"), _attenuation, 10, 40);
    html.writeSlider(CFG_MOD_FREQ, F("Modulation Freq"), F("Hz"), modFreq, 1, 10);
    html.writeSlider(CFG_MOD_DEPTH, F("Modulation Depth"), F("%"), modPercent, 1, 99);
}


void FXFlanger::handleConfigPost(WebServer& webServer)
{
    float msDelay = webServer.arg(CFG_DELAY).toInt();
    _delay = roundf(msDelay * _sampleRate / 1000);

    _attenuation = webServer.arg(CFG_ATTENUATION).toInt();

    int modFreq = webServer.arg(CFG_MOD_FREQ).toInt();
    _modulationPeriod = _sampleRate / modFreq;

    int modPercent = webServer.arg(CFG_MOD_DEPTH).toInt();
    _modulationDepth = _delay * modPercent / 100;

    TRACE(
        F("delay=%0.1f ms, _delay=%u, _attenation=%d\n"),
        msDelay,
        _delay,
        _attenuation
        );
    TRACE(
        F("modFreq=%d Hz, modPercent=%d, _modulationPeriod=%u, _modulationDepth=%u\n"),
        modFreq,
        modPercent,
        _modulationPeriod,
        _modulationDepth
        );
}


void FXFlanger::filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size)
{
    int32_t modulationIndex = index % _modulationPeriod;
    float modulation = _sineTable[SINE_SAMPLES * modulationIndex / _modulationPeriod] * _modulationDepth;

    int32_t delayIndex = index - _delay + modulation;
    if (delayIndex < 0) delayIndex += size;

    int32_t delayedSample = buffer[delayIndex];
    
    newSample += (8 * delayedSample / _attenuation);
}
