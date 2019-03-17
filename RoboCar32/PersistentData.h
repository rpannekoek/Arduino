#include <PersistentDataBase.h>
#include <string.h>
#include <stdint.h>

struct PersistentDataStruct : PersistentDataBase
{
    char hostName[20];
    int16_t timeZoneOffset; // hours

    PersistentDataStruct() 
        : PersistentDataBase(sizeof(hostName) + sizeof(timeZoneOffset)) {}

    void initialize() override
    {
        strcpy(hostName, "RoboCar");
        timeZoneOffset = 1;
    }

    void validate() override
    {
        if (timeZoneOffset < -12) timeZoneOffset = -12;
        if (timeZoneOffset > 14) timeZoneOffset = 14;
    }
};

PersistentDataStruct PersistentData;
