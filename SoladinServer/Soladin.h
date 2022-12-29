#ifndef SOLADIN_H
#define SOLADIN_H

#include <inttypes.h>
#include <WString.h>

class SoladinComm
{
    public:
        String flags;
        float pvVoltage;
        float pvCurrent;
        float gridFrequency;
        int gridVoltage;
        int gridPower;
        float gridEnergy;
        int temperature;
    
        SoladinComm();
        bool test();
        bool probe();
        bool getDeviceStats();

    private:
        const uint8_t _cmdProbe[9] = {00, 00, 00, 00, 0xC1, 00, 00, 00, 0xC1};
        const uint8_t _cmdDeviceStats[9] = {0x11, 00, 00, 00, 0xB6, 00, 00, 00, 0xC7};
        const uint8_t _cmdHistory[9] = {0x11, 00, 00, 00, 0x9A, 00, 00, 00, 0xAB};

        void resetGpio(bool swap);
        bool query(uint8_t* cmd, uint8_t* response, int responseSize);
};
#endif