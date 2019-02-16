#include <PersistentDataBase.h>

struct PersistentDataClass : PersistentDataBase
{
    char HostName[20];
    int8_t TimeZoneOffset; // hours
    uint16_t OpenThermLogInterval; // seconds
    char WeatherApiKey[16];
    char WeatherLocation[16];

    PersistentDataClass() 
        : PersistentDataBase(sizeof(HostName) + sizeof(TimeZoneOffset) + sizeof(OpenThermLogInterval) + sizeof(WeatherApiKey) + sizeof(WeatherLocation)) {}

    void begin()
    {
        if (readFromEEPROM())
        {
            validate();
            return;
        }

        TRACE(F("EEPROM not initialized; initializing with defaults.\n"));

        strcpy(HostName, "OpenThermGateway");
        TimeZoneOffset = 1;
        OpenThermLogInterval = 60;
        WeatherApiKey[0] = 0;
        WeatherLocation[0] = 0; 
    }

    void validate()
    {
        if (TimeZoneOffset < -12) TimeZoneOffset = -12;
        if (TimeZoneOffset > 14) TimeZoneOffset = 14;
        if (OpenThermLogInterval < 5) OpenThermLogInterval = 5;
        if (OpenThermLogInterval > 900) OpenThermLogInterval = 900;
        if (WeatherApiKey[0] == 0xFF) WeatherApiKey[0] = 0;
        if (WeatherLocation[0] == 0xFF) WeatherLocation[0] = 0;
    }
};

PersistentDataClass PersistentData;
