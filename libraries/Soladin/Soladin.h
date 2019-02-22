#ifndef SOLADIN_H
#define SOLADIN_H

#include <inttypes.h>
#include <WString.h>

#ifndef byte
#define byte uint8_t
#endif

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
        const byte _cmdProbe[9] = {00, 00, 00, 00, 0xC1, 00, 00, 00, 0xC1};
        const byte _cmdDeviceStats[9] = {0x11, 00, 00, 00, 0xB6, 00, 00, 00, 0xC7};
        const byte _cmdHistory[9] = {0x11, 00, 00, 00, 0x9A, 00, 00, 00, 0xAB};

        void resetGpio(bool swap);
        bool query(byte* cmd, byte* response, int responseSize);
};
#endif