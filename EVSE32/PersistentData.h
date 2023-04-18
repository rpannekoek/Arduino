#include <OneWire.h>
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
    uint16_t registeredBeaconCount;
    esp_bd_addr_t registeredDevices[MAX_BT_DEVICES]; // Not used
    float currentScale;
    DeviceAddress tempSensorAddress;
    float tempSensorOffset;
    uuid128_t registeredBeacons[MAX_BT_DEVICES];
    uint16_t authorizeTimeout;


    PersistentDataStruct() : PersistentDataBase(
        sizeof(wifiSSID) +
        sizeof(wifiKey) +  
        sizeof(hostName) + 
        sizeof(ntpServer) + 
        sizeof(dsmrMonitor) +
        sizeof(dsmrPhase) +
        sizeof(currentLimit) +
        sizeof(currentZero) +
        sizeof(registeredBeaconCount) +
        sizeof(registeredDevices) +
        sizeof(currentScale) +
        sizeof(tempSensorAddress) +
        sizeof(tempSensorOffset) +
        sizeof(registeredBeacons) +
        sizeof(authorizeTimeout)
        ) {}

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
        registeredBeaconCount = 0;
        memset(registeredBeacons, 0, sizeof(registeredBeacons));
        memset(tempSensorAddress, 0, sizeof(DeviceAddress));
        tempSensorOffset = 0;
        authorizeTimeout = 15 * 60;
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
        registeredBeaconCount = std::min(registeredBeaconCount, (uint16_t)MAX_BT_DEVICES);
        tempSensorOffset = std::min(std::max(tempSensorOffset, -5.0F), 5.0F);
    }
};

PersistentDataStruct PersistentData;
