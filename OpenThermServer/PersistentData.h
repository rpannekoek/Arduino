#include <PersistentDataBase.h>


struct PersistentDataStruct : PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];
    uint16_t boilerOnDelay; // seconds
    uint16_t openThermLogInterval; // Not used
    char weatherApiKey[16];
    char weatherLocation[16];
    uint16_t ftpSyncEntries;
    uint16_t maxTSet;
    uint16_t minTSet;
    char heatmonHost[32];

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(boilerOnDelay) +
        sizeof(openThermLogInterval) +
        sizeof(weatherApiKey) +
        sizeof(weatherLocation) +
        sizeof(ftpSyncEntries) +
        sizeof(maxTSet) +
        sizeof(minTSet) +
        sizeof(heatmonHost)
        ) {}

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "OTGW");
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
        boilerOnDelay = 0;
        openThermLogInterval = 60;
        weatherApiKey[0] = 0;
        weatherLocation[0] = 0; 
        ftpSyncEntries = 0; // FTP sync disabled
        maxTSet = 60;
        minTSet = 40;
        heatmonHost[0] = 0;
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
        heatmonHost[sizeof(heatmonHost) - 1] = 0;

        if (openThermLogInterval < 5) openThermLogInterval = 5;
        if (openThermLogInterval > 900) openThermLogInterval = 900;
        if (weatherApiKey[0] == 0xFF) weatherApiKey[0] = 0;
        if (weatherLocation[0] == 0xFF) weatherLocation[0] = 0;
        if (ftpSyncEntries > 255) ftpSyncEntries = 0;
        if (boilerOnDelay > 3600) boilerOnDelay = 3600;
        maxTSet = std::min(std::max(maxTSet, (uint16_t)40), (uint16_t)80); 
        minTSet = std::min(std::max(minTSet, (uint16_t)20), (uint16_t)40); 
    }
};

PersistentDataStruct PersistentData;
