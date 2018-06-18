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
    String Flags;
    float PvVoltage;
    float PvCurrent;
    float GridFrequency;
    int GridVoltage;
    int GridPower;
    float GridEnergy;
    int Temperature;
  
    SoladinComm();
    bool test();
    bool probe();
    bool getDeviceStats();

   private:
    const byte _cmdProbe[9] = {00, 00, 00, 00, 0xC1, 00, 00, 00, 0xC1};
    const byte _cmdDeviceStats[9] = {0x11, 00, 00, 00, 0xB6, 00, 00, 00, 0xC7};
    const byte _cmdHistory[9] = {0x11, 00, 00, 00, 0x9A, 00, 00, 00, 0xAB};
    const String _flags[12] = {"Vpv+", "Vpv-", "!Vac", "Vac+", "Vac-", "Fac+", "Fac-", "T+", "HW-ERR", "Start", "Pmax", "Imax"};

    void resetGpio(bool swap);
    bool query(byte* cmd, byte* response, int responseSize);
};
#endif