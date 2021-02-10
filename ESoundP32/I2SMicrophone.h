#ifndef I2S_MICROPHONE_H
#define I2S_MICROPHONE_H

#include <driver/i2s.h>
#include "FX.h"

class I2SMicrophone
{
    public:
        // Constructor
        I2SMicrophone(FXEngine& fxEngine) : _fxEngine(fxEngine)
        {
        }

        inline bool isRecording()
        {
            return _isRecording;
        }

        inline uint32_t getRecordedSamples()
        {
            return _recordedSamples;
        }

        inline uint32_t getCycles()
        {
            return _cycles;
        }

        bool begin(i2s_port_t i2sPort, int sampleRate, int bckPin, int wsPin, int dataPin);
        bool startRecording();
        bool stopRecording();
        bool setGain(float dB);
        float getGain();
        float adjustGain(float dBFS);

    protected:
        i2s_port_t _i2sPort;
        FXEngine& _fxEngine;
        TaskHandle_t _dataSinkTaskHandle;
        volatile bool _isRecording = false;
        volatile int32_t _scale = 4096;
        uint32_t _recordedSamples = 0;
        uint32_t _cycles = 0;

        void dataSink();

    private:
        static void dataSinkTask(void* taskParams);
};

#endif
