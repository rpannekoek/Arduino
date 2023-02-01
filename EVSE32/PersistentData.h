#include <PersistentDataBase.h>

#define MAX_BT_DEVICES 4

struct __attribute__ ((packed)) PersistentDataStruct : PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];
    char dsmrMonitor[32];
    uint8_t dsmrPhase;
    uint8_t currentLimit;
    uint16_t currentZero;
    uint16_t registeredDeviceCount;
    esp_bd_addr_t registeredDevices[MAX_BT_DEVICES];
    float currentScale;

    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(dsmrMonitor) +
        sizeof(dsmrPhase) +
        sizeof(currentLimit) +
        sizeof(currentZero) +
        sizeof(registeredDeviceCount) +
        sizeof(registeredDevices) +
        sizeof(currentScale)
        ) {}

    bool isDeviceRegistered(const esp_bd_addr_t& bda)
    {
        for (int i = 0; i < registeredDeviceCount; i++)
        {
            if (memcmp(bda, registeredDevices[i], sizeof(esp_bd_addr_t)) == 0)
                return true;
        }
        return false;
    }

    virtual void initialize()
    {
        wifiSSID[0] = 0;
        wifiKey[0] = 0;
        strcpy(hostName, "EVSE");
        strcpy(ntpServer, "europe.pool.ntp.org");
        dsmrMonitor[0] = 0;
        dsmrPhase = 2; 
        currentLimit = 10;
        currentZero = 2048;
        currentScale = 0.016;
        registeredDeviceCount = 0;
        memset(registeredDevices, 0, sizeof(registeredDevices));
    }

    virtual void validate()
    {
        // Ensure all strings are terminated
        wifiSSID[sizeof(wifiSSID) - 1] = 0;
        wifiKey[sizeof(wifiKey) - 1] = 0;
        hostName[sizeof(hostName) - 1] = 0;
        ntpServer[sizeof(ntpServer) - 1] = 0;
        dsmrMonitor[sizeof(dsmrMonitor) - 1] = 0;

        dsmrPhase = std::min(dsmrPhase, (uint8_t)2);
        currentLimit = std::min(std::max(currentLimit, (uint8_t)6), (uint8_t)25);
        registeredDeviceCount = std::min(registeredDeviceCount, (uint16_t)MAX_BT_DEVICES);
    }
};

PersistentDataStruct PersistentData;
