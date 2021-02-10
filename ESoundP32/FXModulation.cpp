#include <Arduino.h>
#include <Tracer.h>
#include "FXModulation.h"

#define CFG_MOD_FREQ F("Mod_Freq")


void FXModulation::initialize()
{
    _modulationPeriod = 16;
    _modulationIndex = 0;
    buildSineTable();
}


void FXModulation::buildSineTable()
{
    if (_sineTable != nullptr) free(_sineTable);

    _sineTable = (float*) ps_malloc(_modulationPeriod * sizeof(float));
    for (int i = 0; i < _modulationPeriod; i++)
    {
        float phi = 2.0 * PI * i / _modulationPeriod;
        _sineTable[i] = sinf(phi);
    }
}


void FXModulation::writeConfigForm(HtmlWriter& html)
{
    int modulationFreqKHz = (_sampleRate / _modulationPeriod) / 1000;

    html.writeSlider(CFG_MOD_FREQ, F("Frequency"), F("kHz"), modulationFreqKHz, 1, 10);
}


void FXModulation::handleConfigPost(WebServer& webServer)
{
    int modulationFreqKHz = webServer.arg(CFG_MOD_FREQ).toInt();
    _modulationPeriod = _sampleRate / (modulationFreqKHz * 1000);
    _modulationIndex = 0;
    buildSineTable();

    TRACE(F("freq=%d kHz, _modulationPeriod=%u\n"), modulationFreqKHz, _modulationPeriod);
}


int32_t FXModulation::filter(int32_t sample, WaveBuffer& inputBuffer, WaveBuffer& outputBuffer)
{
    if (++_modulationIndex == _modulationPeriod) _modulationIndex = 0;
    return float(sample) * _sineTable[_modulationIndex];
}
