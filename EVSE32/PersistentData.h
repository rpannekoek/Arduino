#include <OneWire.h>
#include <PersistentDataBase.h>

constexpr size_t MAX_BT_DEVICES = 4;

struct Settings : WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    char dsmrMonitor[32];
    int dsmrPhase;
    int currentLimit;
    int authorizeTimeout;
    int tempLimit;
    float tempSensorOffset;
    DeviceAddress tempSensorAddress;
    float currentScale;
    uint16_t currentZero;
    uint16_t registeredBeaconCount;
    uuid128_t registeredBeacons[MAX_BT_DEVICES];

    static constexpr size_t ADDITIONAL_DATA_SIZE =
        sizeof(tempSensorAddress) +
        sizeof(currentScale) +
        sizeof(currentZero) +
        sizeof(registeredBeaconCount) +
        sizeof(registeredBeacons);

    Settings() : WiFiSettingsWithFTP("EVSE", ADDITIONAL_DATA_SIZE)
    {
        addIntegerField(ftpSyncEntries, "FTP Sync Entries", 0, 200);
        addStringField(dsmrMonitor, sizeof(dsmrMonitor), "DSMR Monitor");
        addIntegerField(dsmrPhase, "DSMR Phase", 1, 3, 3);
        addIntegerField(currentLimit, "Current Limit", 6, 25, 16);
        addTimeSpanField(authorizeTimeout, "Authorize Timeout", 0, 3600, 15 * 60);
        addIntegerField(tempLimit, "Temperature Limit", 40, 60, 50);
        addFloatField(tempSensorOffset, "Temperature Offset", 1, -5.0, 5.0);
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
        memset(tempSensorAddress, 0, sizeof(DeviceAddress));
        currentScale = 0.016;
        currentZero = 2048;
        registeredBeaconCount = 0;
        memset(registeredBeacons, 0, sizeof(registeredBeacons));
    }

    void validate() override
    {
        WiFiSettingsWithFTP::validate();
        registeredBeaconCount = std::min(registeredBeaconCount, (uint16_t)MAX_BT_DEVICES);
        tempSensorOffset = std::min(std::max(tempSensorOffset, -5.0F), 5.0F);
    }
};

Settings PersistentData;
