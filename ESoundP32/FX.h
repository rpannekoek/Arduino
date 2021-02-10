#ifndef FX_H
#define FX_H

#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <ESPWebServer.h>
#include "WaveBuffer.h"

#define MAX_FX 8

class SoundEffect
{
    public:
        inline bool isEnabled()
        {
            return _isEnabled;
        }

        virtual String getName() = 0;
        virtual void initialize() = 0;
        virtual void writeConfigForm(HtmlWriter& html) = 0;
        virtual void handleConfigPost(WebServer& webServer) = 0;
        virtual int32_t filter(int32_t sample, WaveBuffer& inputBuffer, WaveBuffer& outputBuffer) = 0;

    protected:
        uint16_t _sampleRate;

    private:
        bool _isEnabled = false;

        friend class FXEngine;
};


class FXEngine
{
    public:
        FXEngine(WaveBuffer& outputBuffer) : _outputBuffer(outputBuffer)
        {
        }

        inline SoundEffect* getSoundEffect(uint16_t id)
        {
            if (id >= _numRegisteredFX) return nullptr;
            return _registeredFX[id];
        }

        inline int getNumRegisteredFX()
        {
            return _numRegisteredFX;
        }

        inline void addSample(int32_t sample)
        {
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
        }

        bool begin(uint16_t sampleRate);
        bool add(SoundEffect* fx);
        bool enable(SoundEffect* fx);
        bool reset();

    private:
        WaveBuffer& _outputBuffer;
        WaveBuffer _inputBuffer;
        SoundEffect* _registeredFX[MAX_FX];
        SoundEffect* _enabledFX[MAX_FX];
        int _numRegisteredFX = 0;
        int _numEnabledFX = 0;
        uint16_t _sampleRate;
};

#endif