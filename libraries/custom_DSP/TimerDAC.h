#ifndef I2S_DAC_H
#define I2S_DAC_H

#include <driver/dac.h>
#include "WaveBuffer.h"

class TimerDAC
{
    public:
        // Constructor
        TimerDAC(WaveBuffer& waveBuffer) : _waveBuffer(waveBuffer)
        {
        }

        inline bool isPlaying()
        {
            return _isPlaying;
        }

        bool begin(dac_channel_t dacChannel, uint16_t sampleRate);
        bool startPlaying();
        bool stopPlaying();

    protected:
        WaveBuffer& _waveBuffer;
        dac_channel_t _dacChannel;
        hw_timer_t* _timer;
        static TaskHandle_t _dataSourceTaskHandle;
        bool _isPlaying = false;

        void dataSource();

    private:
        static void dataSourceTask(void* taskParams);
        static void IRAM_ATTR timerISR();
};

#endif
