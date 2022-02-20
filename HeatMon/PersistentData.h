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
    DeviceAddress tInputSensorAddress;
    DeviceAddress tOutputSensorAddress;
    DeviceAddress tBufferSensorAddress;
    float tInputOffset;
    float tOutputOffset;
    float tBufferOffset;
    float tBufferMax;

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(ftpServer) + 
        sizeof(ftpUser) + 
        sizeof(ftpPassword) + 
        sizeof(timeZoneOffset) +
        sizeof(tInputSensorAddress) +
        sizeof(tOutputSensorAddress) +
        sizeof(tBufferSensorAddress) +
        sizeof(tInputOffset) +
        sizeof(tOutputOffset) +
        sizeof(tBufferOffset) +
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
        memset(tInputSensorAddress, 0, sizeof(tInputSensorAddress));
        memset(tOutputSensorAddress, 0, sizeof(tOutputSensorAddress));
        memset(tBufferSensorAddress, 0, sizeof(tOutputSensorAddress));
        tInputOffset = 0;
        tOutputOffset = 0;
        tBufferOffset = 0;
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

        if (timeZoneOffset < -12) timeZoneOffset = -12;
        if (timeZoneOffset > 14) timeZoneOffset = 14;

        tInputOffset = std::max(std::min(tInputOffset, 2.0F), -2.0F);
        tOutputOffset = std::max(std::min(tOutputOffset, 2.0F), -2.0F);
        tBufferOffset = std::max(std::min(tBufferOffset, 2.0F), -2.0F);

        if (tBufferMax != 0)
            tBufferMax = std::max(std::min(tBufferMax, 100.0F), 80.0F);
    }
};

PersistentDataStruct PersistentData;
