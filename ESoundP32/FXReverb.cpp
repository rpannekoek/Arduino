#include <Arduino.h>
#include <Tracer.h>
#include "FXReverb.h"

#define CFG_DELAY F("Reverb_Delay")
#define CFG_ATTENUATION F("Reverb_Att")

void FXReverb::initialize()
{
    _delay = _sampleRate / 5; // 200 ms
    _attenuation = 80;
}


void FXReverb::writeConfigForm(HtmlWriter& html)
{
    int msDelay = roundf(1000.0 * _delay / _sampleRate); 

    html.writeSlider(CFG_DELAY, F("Delay"), F("ms"), msDelay, 2, 2000);
    html.writeSlider(CFG_ATTENUATION, F("Attenuation"), F("x"), _attenuation, 10, 80, 8);
}


void FXReverb::handleConfigPost(WebServer& webServer)
{
    float msDelay = webServer.arg(CFG_DELAY).toInt();
    _delay = roundf(msDelay * _sampleRate / 1000);
    _attenuation = webServer.arg(CFG_ATTENUATION).toInt();

    TRACE(F("delay=%0.1f ms, _delay=%u, _attenation=%d\n"), msDelay, _delay, _attenuation);
}


int32_t FXReverb::filter(int32_t sample, WaveBuffer& inputBuffer, WaveBuffer& outputBuffer)
{
    int32_t delayedSample = outputBuffer.getSample(_delay);
    return sample + (8 * delayedSample / _attenuation);
}
