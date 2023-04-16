#include <PersistentDataBase.h>

struct PersistentDataStruct : PersistentDataBase
{
    char hostName[32];
    char wifiSSID[32];
    char wifiKey[32];
    char ntpServer[32];
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];

    PersistentDataStruct() : PersistentDataBase(
        sizeof(hostName) +
        sizeof(wifiSSID) +
        sizeof(wifiKey) +
        sizeof(ntpServer) +
        sizeof(ftpServer) +
        sizeof(ftpUser) +
        sizeof(ftpPassword)
        ) {}

    inline bool isFTPEnabled()
    {
        return ftpServer[0] != 0;
    }

    virtual void initialize()
    {
        strcpy(hostName, "SoladinServer");
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(ntpServer, "europe.pool.ntp.org");
        ftpServer[0] = 0;
        ftpUser[0] = 0;
        ftpPassword[0] = 0;
    }

    virtual void validate()
    {
        // Ensure all strings are terminated
        hostName[sizeof(hostName) - 1] = 0;
        hostName[sizeof(wifiSSID) - 1] = 0;
        hostName[sizeof(wifiKey) - 1] = 0;
        hostName[sizeof(ntpServer) - 1] = 0;
        hostName[sizeof(ftpServer) - 1] = 0;
        hostName[sizeof(ftpUser) - 1] = 0;
        hostName[sizeof(ftpPassword) - 1] = 0;
    }
};

PersistentDataStruct PersistentData;
