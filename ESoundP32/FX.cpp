#include <Tracer.h>
#include "FX.h"


bool FXEngine::registerFX(SoundEffect* fx)
{
    if (_numRegisteredFX == MAX_FX) return false;
    _registeredFX[_numRegisteredFX++] = fx;
    return true;
}


bool FXEngine::enableFX(SoundEffect* fx)
{
    Tracer tracer(F("FXEngine::enableFX"), fx->getName().c_str());

    if (fx->_isEnabled)
    {
        TRACE(F("Sound Effect '%s' is already enabled.\n"), fx->getName().c_str());
        return false;
    }

    fx->_isEnabled = true;
    _enabledFX[_numEnabledFX++] = fx;
    return true;
}


bool FXEngine::resetFX()
{
    _numEnabledFX = 0;

    for (int i = 0; i < _numRegisteredFX; i++)
    {
        _registeredFX[i]->_isEnabled = false;
    }

    return true;    
}
