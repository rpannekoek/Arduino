#ifndef FLOWSENSOR_H
#define FLOWSENSOR_H

#include <Arduino.h>
#include <Ticker.h>

class FlowSensor
{
    public:
        FlowSensor(uint8_t pin);

        bool begin(float measureInterval, float pulseFreq);
        void end();

        float getFlowRate() const
        {
            return _flowRate;
        }

    private:
        static uint8_t _pinInterrupt;
        static float _measureInterval;
        static float _pulseFreq;
        static uint32_t _pulseCount;
        static float _flowRate;
        Ticker _ticker;

        ICACHE_RAM_ATTR static void pulseISR();
        static void measure(); 
};


#endif