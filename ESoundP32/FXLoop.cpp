#include <Arduino.h>
#include <Tracer.h>
#include "FXLoop.h"

#define MAX_TICK_AMPLITUDE 16384

#define CFG_BPM F("Loop_BPM")
#define CFG_BEATS F("Loop_Beats")
#define CFG_ATT F("Loop_Att")
#define CFG_TICK_VOL F("Loop_TickVol")
#define CFG_TICK_MS F("Loop_TickMS")

void FXLoop::initialize()
{
    _bpm = 120;
    _loopBeats = 4;
    _attenuation = 18;
    _beatLength = _sampleRate * 60 / _bpm;
    _delay = _loopBeats * _beatLength;
    _tickAmplitude = MAX_TICK_AMPLITUDE / 2;
    _tickPulseWidth = _sampleRate / 1000;
    _tickIndex = 0;
    _tickCount = 0;
}


void FXLoop::writeConfigForm(HtmlWriter& html)
{
    int tickVolumePct = 100 * _tickAmplitude / MAX_TICK_AMPLITUDE;
    int msTickPulseWidth = 1000 * _tickPulseWidth / _sampleRate;

    html.writeSlider(CFG_BPM, F("BPM"), String(), _bpm, 60, 180);
    html.writeSlider(CFG_BEATS, F("Beats"), String(), _loopBeats, 3, 16, 1);
    html.writeSlider(CFG_ATT, F("Attenuation"), String(), _attenuation, 16, 32, 16);
    html.writeSlider(CFG_TICK_VOL, F("Tick Volume"), F("%"), tickVolumePct, 0, 100, 1);
    html.writeSlider(CFG_TICK_MS, F("Tick Width"), F("ms"), msTickPulseWidth, 1, 10, 1);
}


void FXLoop::handleConfigPost(WebServer& webServer)
{
    _bpm = webServer.arg(CFG_BPM).toInt();
    _loopBeats = webServer.arg(CFG_BEATS).toInt();
    _attenuation = webServer.arg(CFG_ATT).toInt();
    int tickVolumePct = webServer.arg(CFG_TICK_VOL).toInt();
    int msTickPulseWidth = webServer.arg(CFG_TICK_MS).toInt();

    _beatLength = _sampleRate * 60 / _bpm;
    _delay = _loopBeats * _beatLength;
    _tickAmplitude = MAX_TICK_AMPLITUDE * tickVolumePct / 100;
    _tickPulseWidth = _sampleRate * msTickPulseWidth / 1000;

    TRACE(
        F("bpm=%d, loopBeats=%d, att=%d. _beatLength=%u, _delay=%u, _tickAmplitude=%d, _tickPulseWidth=%u\n"),
        _bpm, _loopBeats, _attenuation, _beatLength, _delay, _tickAmplitude, _tickPulseWidth
        );

    _tickIndex = 0;
    _tickCount = 0;
}


int32_t FXLoop::filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer)
{
    int32_t delayedSample = outputBuffer.getSample(_delay);

    int32_t tick = 0;
    if ((_tickAmplitude > 0) && (_tickCount < _loopBeats))
    {
        if (_tickIndex++ < _tickPulseWidth) tick = _tickAmplitude;
        if (_tickIndex >= _beatLength)
        {
            _tickIndex = 0;
            _tickCount++;
        }
    }

    return sample + (delayedSample * 16 / _attenuation ) + tick;
}
