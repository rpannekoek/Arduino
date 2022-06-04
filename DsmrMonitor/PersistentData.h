#include <PersistentDataBase.h>

#define DEFAULT_GAS_KWH_PER_M3 9.769

struct PersistentDataStruct : PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];
    uint16_t ftpSyncEntries;
    uint16_t phaseCount; // 1 or 3
    uint16_t maxPhaseCurrent; // A (per phase)
    uint16_t powerLogDelta;
    float gasCalorificValue; // kWh per m3

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(ftpSyncEntries) +
        sizeof(phaseCount) +
        sizeof(maxPhaseCurrent) +
        sizeof(powerLogDelta) +
        sizeof(gasCalorificValue)
        ) {}

    inline bool isFTPEnabled()
    {
        return ftpSyncEntries != 0;
    }

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "DsmrMonitor");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        ftpSyncEntries = 0;
        phaseCount = 1;
        maxPhaseCurrent = 35;
        gasCalorificValue = DEFAULT_GAS_KWH_PER_M3;
        powerLogDelta = 10;
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

        if ((phaseCount != 1) && phaseCount !=3) phaseCount = 1;
        if (maxPhaseCurrent < 25) maxPhaseCurrent = 25;
        if (maxPhaseCurrent > 75) maxPhaseCurrent = 75;
        if ((gasCalorificValue < 1) || (gasCalorificValue > 15)) gasCalorificValue = DEFAULT_GAS_KWH_PER_M3; 
        if (powerLogDelta > 1000) powerLogDelta = 1000;
    }
};

PersistentDataStruct PersistentData;
