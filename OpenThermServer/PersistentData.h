#include <PersistentDataBase.h>


struct PersistentDataStruct : PersistentDataBase
{
    char hostName[20];
    int16_t timeZoneOffset; // hours
    uint16_t openThermLogInterval; // seconds
    char weatherApiKey[16];
    char weatherLocation[16];
    uint16_t ftpSyncInterval; // number of log entries

    PersistentDataStruct() 
        : PersistentDataBase(sizeof(hostName) + sizeof(timeZoneOffset) + sizeof(openThermLogInterval) + sizeof(weatherApiKey) + sizeof(weatherLocation) + sizeof(ftpSyncInterval)) {}

    virtual void initialize()
    {
        strcpy(hostName, "OpenThermGateway");
        timeZoneOffset = 1;
        openThermLogInterval = 60;
        weatherApiKey[0] = 0;
        weatherLocation[0] = 0; 
        ftpSyncInterval = 0; // FTP sync disabled
    }

    virtual void validate()
    {
        if (timeZoneOffset < -12) timeZoneOffset = -12;
        if (timeZoneOffset > 14) timeZoneOffset = 14;
        if (openThermLogInterval < 5) openThermLogInterval = 5;
        if (openThermLogInterval > 900) openThermLogInterval = 900;
        if (weatherApiKey[0] == 0xFF) weatherApiKey[0] = 0;
        if (weatherLocation[0] == 0xFF) weatherLocation[0] = 0;
    }
};

PersistentDataStruct PersistentData;
