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
    uint16_t antiFreezeTemp;
    bool logPacketErrors;
    float zone1Offset;
    char otgwHost[32];


    bool inline ftpIsEnabled()
    {
        return ftpSyncEntries > 0;
    }

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) +
        sizeof(ftpSyncEntries)  +
        sizeof(antiFreezeTemp) +
        sizeof(logPacketErrors) + 
        sizeof(zone1Offset) +
        sizeof(otgwHost)
        ) {}

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "AquaMon");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        ftpSyncEntries = 0;
        antiFreezeTemp = 5;
        logPacketErrors = false;
        zone1Offset = 0;
        otgwHost[0] = 0;
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
        otgwHost[sizeof(otgwHost) - 1] = 0;

        ftpSyncEntries = std::min(ftpSyncEntries, (uint16_t)250);
        antiFreezeTemp = std::min(antiFreezeTemp, (uint16_t)10);
        zone1Offset = std::max(std::min(zone1Offset, 5.0F), -5.0F);
    }
};

PersistentDataStruct PersistentData;
