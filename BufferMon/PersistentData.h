#include <PersistentDataBase.h>
#include <DallasTemperature.h>

struct __attribute__ ((packed)) PersistentDataStruct : PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];
    int16_t timeZoneOffset; // hours
    DeviceAddress tempSensorAddress;
    float tempOffset;
    float maxTemp;

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(timeZoneOffset) +
        sizeof(tempSensorAddress) +
        sizeof(tempOffset) +
        sizeof(maxTemp)
        ) {}

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "BufferMon");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        timeZoneOffset = 1;
        memset(tempSensorAddress, 0, sizeof(tempSensorAddress));
        tempOffset = 0;
        maxTemp = 90;
    }

    virtual void validate()
    {
        // Ensure all strings are terminated
        wifiSSID[sizeof(wifiSSID) - 1] = 0;
        wifiKey[sizeof(wifiKey) - 1] = 0;
        hostName[sizeof(hostName) - 1] = 0;
        ntpServer[sizeof(ntpServer) - 1] = 0;
        ftpServer[sizeof(ftpServer) - 1] = 0;
        ftpUser[sizeof(ftpUser) - 1] = 0;
        ftpPassword[sizeof(ftpPassword) - 1] = 0;

        timeZoneOffset = std::max(std::min(timeZoneOffset, (int16_t)14), (int16_t)-12);
        tempOffset = std::max(std::min(tempOffset, 1.0f), -1.0f);
        maxTemp = std::max(std::min(maxTemp, 100.0f), 80.0f);
    }
};

PersistentDataStruct PersistentData;
