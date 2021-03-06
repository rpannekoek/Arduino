#ifndef I2S_DAC_H
#define I2S_DAC_H

#include <driver/i2s.h>
#include "WaveBuffer.h"

class I2SDAC
{
    public:
        // Constructor for internal DAC
        I2SDAC(WaveBuffer& waveBuffer, int sampleRate, i2s_port_t i2sPort, uint8_t timingPin = 0xFF);

        // Constructor for external DAC
        I2SDAC(WaveBuffer& waveBuffer, int sampleRate, i2s_port_t i2sPort, int bckPin, int wsPin, int dataPin, uint8_t timingPin = 0xFF);

        inline bool isPlaying()
        {
            return _isPlaying;
        }

        bool begin();
        bool startPlaying();
        bool stopPlaying();

    protected:
        i2s_port_t _i2sPort;
        i2s_config_t _i2sConfig;
        i2s_pin_config_t* _i2sPinConfig;
        uint8_t _timingPin;
        WaveBuffer& _waveBuffer;
        int16_t* _sampleBuffer;
        TaskHandle_t _dataSourceTaskHandle;
        volatile bool _isPlaying = false;

        void dataSource();

    private:
        static void dataSourceTask(void* taskParams);
};

#endif
