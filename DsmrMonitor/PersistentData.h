#include <PersistentDataBase.h>


struct PersistentDataStruct : PersistentDataBase
{
    char hostName[20];
    int16_t timeZoneOffset; // hours

    PersistentDataStruct() 
        : PersistentDataBase(sizeof(hostName) + sizeof(timeZoneOffset)) {}

    virtual void initialize()
    {
        strcpy(hostName, "DsmrMonitor");
        timeZoneOffset = 1;
    }

    virtual void validate()
    {
        if (timeZoneOffset < -12) timeZoneOffset = -12;
        if (timeZoneOffset > 14) timeZoneOffset = 14;
    }
};

PersistentDataStruct PersistentData;
