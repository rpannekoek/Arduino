#ifndef I2S_MICROPHONE_H
#define I2S_MICROPHONE_H

#include <driver/i2s.h>
#include "FX.h"

class I2SMicrophone
{
    public:
        // Constructor
        I2SMicrophone(ISampleStore& sampleStore, int sampleRate, i2s_port_t i2sPort, int bckPin, int wsPin, int dataPin);

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

        bool begin();
        bool startRecording();
        bool stopRecording();
        bool setGain(float dB);
        float getGain();
        float adjustGain(float dBFS);

    private:
        i2s_port_t _i2sPort;
        i2s_config_t _i2sConfig; 
        i2s_pin_config_t _i2sPinConfig;
        ISampleStore& _sampleStore;
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
