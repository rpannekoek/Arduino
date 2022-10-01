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
    DeviceAddress tInsideSensorAddress;
    DeviceAddress tOutsideSensorAddress;
    float tInsideOffset;
    float tOutsideOffset;
    float tInsideNightOffset;
    uint16_t ftpSyncEntries;

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(tInsideSensorAddress) +
        sizeof(tOutsideSensorAddress) +
        sizeof(tInsideOffset) +
        sizeof(tOutsideOffset) +
        sizeof(tInsideNightOffset) +
        sizeof(ftpSyncEntries)
        ) {}

    bool isFTPEnabled()
    {
        return (ftpSyncEntries > 0);
    }

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "TempMon");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        memset(tInsideSensorAddress, 0, sizeof(tInsideSensorAddress));
        memset(tOutsideSensorAddress, 0, sizeof(tOutsideSensorAddress));
        tInsideOffset = 0;
        tOutsideOffset = 0;
        tInsideNightOffset = 0;
        ftpSyncEntries = 0;
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

        tInsideOffset = std::max(std::min(tInsideOffset, 10.0F), -10.0F);
        tOutsideOffset = std::max(std::min(tOutsideOffset, 10.0F), -10.0F);
        tInsideNightOffset = std::max(std::min(tInsideNightOffset, 10.0F), -10.0F);
    }
};

PersistentDataStruct PersistentData;
