#ifndef ENERGYMETER_H
#define ENERGYMETER_H

#include <Arduino.h>
#include <Ticker.h>

class EnergyMeter
{
    public:
        EnergyMeter(uint8_t pin);

        bool begin(uint16_t resolutionWatt, uint16_t pulsesPerKWh);
        void end();

        float getPower() const // Watts
        {
            return _power;
        }

        float getEnergy() const // kWh
        {
            return float(_energyPulseCount) / _pulsesPerKWh;
        }

        void resetEnergy()
        {
            _energyPulseCount = 0;
        }

    private:
        static uint8_t _pinInterrupt;
        static uint16_t _pulsesPerKWh;
        static float _measureInterval;
        static uint32_t _lastPulseMillis;
        static uint32_t _pulseCount;
        static uint32_t _energyPulseCount;
        static float _power;
        Ticker _ticker;

        ICACHE_RAM_ATTR static void pulseISR();
        static void measure(); 
};


#endif