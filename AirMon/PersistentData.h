#include <PersistentDataBase.h>

struct __attribute__ ((packed)) PersistentDataStruct : PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];
    uint16_t ftpSyncEntries;
    uint16_t fanIAQThreshold;
    uint16_t fanIAQHysteresis;

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(ftpSyncEntries) +
        sizeof(fanIAQThreshold) +
        sizeof(fanIAQHysteresis)
        ) {}

    bool isFTPEnabled()
    {
        return (ftpSyncEntries > 0);
    }

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "AirMon");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        ftpSyncEntries = 0;

        fanIAQThreshold = 100;
        fanIAQHysteresis = 10;
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

        fanIAQThreshold = std::min(fanIAQThreshold, (uint16_t)500);
        fanIAQHysteresis = std::min(fanIAQHysteresis, fanIAQThreshold);
    }
};

PersistentDataStruct PersistentData;
