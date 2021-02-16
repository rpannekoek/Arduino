#ifndef I2S_DAC_H
#define I2S_DAC_H

#include <driver/i2s.h>
#include "WaveBuffer.h"

#define DAC_BUFFER_SAMPLES 256

class I2SDAC
{
    public:
        // Constructor
        I2SDAC(WaveBuffer& waveBuffer) : _waveBuffer(waveBuffer)
        {
        }

        inline bool isPlaying()
        {
            return _isPlaying;
        }

        bool begin(i2s_port_t i2sPort, int sampleRate);
        bool startPlaying();
        bool stopPlaying();

    protected:
        i2s_port_t _i2sPort;
        WaveBuffer& _waveBuffer;
        int16_t* _sampleBuffer;
        TaskHandle_t _dataSourceTaskHandle;
        volatile bool _isPlaying = false;

        void dataSource();

    private:
        static void dataSourceTask(void* taskParams);
};

#endif
