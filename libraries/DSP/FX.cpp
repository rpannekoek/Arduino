#include <Tracer.h>
#include "FX.h"


bool FXEngine::begin(uint16_t sampleRate)
{
    _sampleRate = sampleRate;
    _inputBuffer.begin(sampleRate / 10); // 100 ms input buffer
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
