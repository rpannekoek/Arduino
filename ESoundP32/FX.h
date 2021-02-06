#ifndef FX_H
#define FX_H

#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <ESPWebServer.h>

#define MAX_FX 8

class SoundEffect
{
    public:
        inline bool isEnabled()
        {
            return _isEnabled;
        }

        virtual String getName() = 0;
        virtual void writeConfigForm(HtmlWriter& html) = 0;
        virtual void handleConfigPost(WebServer& webServer) = 0;
        virtual void filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size) = 0;

    private:
        bool _isEnabled = false;

        friend class FXEngine;
};


class FXEngine
{
    public:
        inline SoundEffect* getSoundEffect(uint16_t id)
        {
            if (id >= _numRegisteredFX) return nullptr;
            return _registeredFX[id];
        }

        inline int getNumRegisteredFX()
        {
            return _numRegisteredFX;
        }

        inline void filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size)
        {
            for (int i = 0; i < _numEnabledFX; i++)
            {
                _enabledFX[i]->filter(newSample, buffer, index, size);
            }
        }

        bool registerFX(SoundEffect* fx);
        bool enableFX(SoundEffect* fx);
        bool resetFX();

    private:
        SoundEffect* _registeredFX[MAX_FX];
        SoundEffect* _enabledFX[MAX_FX];
        int _numRegisteredFX = 0;
        int _numEnabledFX = 0;
};

#endif