#include <Tracer.h>
#include "FX.h"


bool FXEngine::begin()
{
    _inputBuffer.begin(_sampleRate / 10); // 100 ms input buffer
}


bool FXEngine::add(SoundEffect* fx)
{
    if (_numRegisteredFX == MAX_FX) return false;

    fx->_sampleRate = _sampleRate;
    fx->initialize();

    _registeredFX[_numRegisteredFX++] = fx;
    return true;
}


bool FXEngine::enable(SoundEffect* fx)
{
    Tracer tracer(F("FXEngine::enable"), fx->getName().c_str());

    if (fx->_isEnabled)
    {
        TRACE(F("Sound Effect '%s' is already enabled.\n"), fx->getName().c_str());
        return false;
    }

    fx->_isEnabled = true;
    _enabledFX[_numEnabledFX++] = fx;
    return true;
}


bool FXEngine::reset()
{
    _numEnabledFX = 0;

    for (int i = 0; i < _numRegisteredFX; i++)
    {
        _registeredFX[i]->_isEnabled = false;
    }

    return true;    
}


void FXEngine::addSample(int32_t sample)
{
    if (_timingPin >= 0) digitalWrite(_timingPin, 0);

    int32_t filteredSample = sample;
    if (_numEnabledFX > 0)
    {
        for (int i = 0; i < _numEnabledFX; i++)
        {
            filteredSample = _enabledFX[i]->filter(filteredSample, _inputBuffer, _outputBuffer);
        }
        _inputBuffer.addSample(sample);
    }
    _outputBuffer.addSample(filteredSample);

    if (_timingPin >= 0) digitalWrite(_timingPin, 1);
}


int16_t FXEngine::getSample(uint32_t delay)
{
    _outputBuffer.getSample(delay);
}
