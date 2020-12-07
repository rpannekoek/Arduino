#include <PersistentDataBase.h>

#define DEFAULT_GAS_KWH_PER_M3 9.769

struct PersistentDataStruct : PersistentDataBase
{
    char hostName[20];
    int16_t timeZoneOffset; // hours
    uint16_t phaseCount; // 1 or 3
    uint16_t maxPhaseCurrent; // A (per phase)
    float gasCalorificValue; // kWh per m3

    PersistentDataStruct() 
        : PersistentDataBase(sizeof(hostName) + sizeof(timeZoneOffset)) {}

    virtual void initialize()
    {
        strcpy(hostName, "DsmrMonitor");
        timeZoneOffset = 1;
        phaseCount = 1;
        maxPhaseCurrent = 35;
        gasCalorificValue = DEFAULT_GAS_KWH_PER_M3;
    }

    virtual void validate()
    {
        if (timeZoneOffset < -12) timeZoneOffset = -12;
        if (timeZoneOffset > 14) timeZoneOffset = 14;
        if ((phaseCount != 1) && phaseCount !=3) phaseCount = 1;
        if (maxPhaseCurrent < 25) maxPhaseCurrent = 25;
        if (maxPhaseCurrent > 75) maxPhaseCurrent = 75;
        if ((gasCalorificValue < 1) || (gasCalorificValue > 15)) gasCalorificValue = DEFAULT_GAS_KWH_PER_M3; 
    }
};

PersistentDataStruct PersistentData;
