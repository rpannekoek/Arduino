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
    int16_t timeZoneOffset; // Not used
    DeviceAddress tempSensorAddress[3];
    float tempSensorOffset[3];
    float tBufferMax;

    bool inline isFTPEnabled()
    {
        return ftpServer[0] != 0;
    }

    bool inline isBufferEnabled()
    {
        return tBufferMax != 0;
    }

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
        sizeof(tempSensorOffset) +
        sizeof(tBufferMax)
        ) {}

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "HeatMon");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        timeZoneOffset = 1;
        memset(tempSensorAddress, 0, sizeof(tempSensorAddress));
        memset(tempSensorOffset, 0, sizeof(tempSensorOffset));
        tBufferMax = 0;
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

        for (int i = 0; i < 3; i++)
        {
            tempSensorOffset[i] = std::max(std::min(tempSensorOffset[i], 2.0F), -2.0F);
        }

        if (tBufferMax != 0)
            tBufferMax = std::max(std::min(tBufferMax, 100.0F), 80.0F);
    }
};

PersistentDataStruct PersistentData;
