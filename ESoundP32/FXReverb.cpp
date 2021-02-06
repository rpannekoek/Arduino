#include <Arduino.h>
#include <Tracer.h>
#include "FXReverb.h"

#define CFG_DELAY F("Reverb_Delay")
#define CFG_ATTENUATION F("Reverb_Att")

// Constructor
FXReverb::FXReverb(uint16_t sampleRate)
{
    _sampleRate = sampleRate;
    _delay = _sampleRate / 5; // 200 ms
    _attenuation = 80;
}


void FXReverb::writeConfigForm(HtmlWriter& html)
{
    int msDelay = roundf(1000.0 * _delay / _sampleRate); 

    html.writeSlider(CFG_DELAY, F("Delay"), F("ms"), msDelay, 2, 2000);
    html.writeSlider(CFG_ATTENUATION, F("Attenuation"), F("x"), _attenuation, 10, 80);
}


void FXReverb::handleConfigPost(WebServer& webServer)
{
    float msDelay = webServer.arg(CFG_DELAY).toInt();
    _delay = roundf(msDelay * _sampleRate / 1000);
    _attenuation = webServer.arg(CFG_ATTENUATION).toInt();

    TRACE(F("delay=%0.1f ms, _delay=%u, _attenation=%d\n"), msDelay, _delay, _attenuation);
}


void FXReverb::filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size)
{
    int32_t delayIndex = index - _delay;
    if (delayIndex < 0) delayIndex += size;

    int32_t delayedSample = buffer[delayIndex];
    
    newSample += (8 * delayedSample / _attenuation);

    /*
    TRACE(
        F("newSample: %d, delayedSample: %d, delayIndex: %u, index: %u, size: %u\n"),
        newSample,
        delayedSample,
        delayIndex,
        index,
        size
        );
    */
}
