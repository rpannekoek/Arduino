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
        virtual int32_t filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer) = 0;

    protected:
        uint16_t _sampleRate;

    private:
        bool _isEnabled = false;

        friend class FXEngine;
};


class FXEngine : public ISampleBuffer
{
    public:
        FXEngine(WaveBuffer& outputBuffer, uint16_t sampleRate, uint8_t timingPin = 0xFF) 
            : _outputBuffer(outputBuffer), _sampleRate(sampleRate), _timingPin(timingPin)
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

        bool begin();
        bool add(SoundEffect* fx);
        bool enable(SoundEffect* fx);
        bool reset();
        virtual void addSample(int32_t sample);
        virtual void addSamples(int32_t* samples, uint32_t numSamples);
        virtual int16_t getSample(uint32_t delay);

    private:
        WaveBuffer& _outputBuffer;
        WaveBuffer _inputBuffer;
        SoundEffect* _registeredFX[MAX_FX];
        SoundEffect* _enabledFX[MAX_FX];
        int _numRegisteredFX = 0;
        int _numEnabledFX = 0;
        uint16_t _sampleRate;
        uint8_t _timingPin;
};

#endif