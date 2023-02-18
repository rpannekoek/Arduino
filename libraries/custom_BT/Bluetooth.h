#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <esp_bt_defs.h>
#include <vector>
#include "UUID.h"

#if !defined(CONFIG_BT_ENABLED)
    #error Bluetooth is not enabled!
#endif

enum struct BluetoothState
{
    Uninitialized = 0,
    Initialized,
    Discovering,
    DiscoveryComplete,
    Authenticated,
    AuthenticationFailed
};


struct BluetoothDeviceInfo
{
    esp_bd_addr_t address;
    char name[16];
    int8_t rssi;
    uint16_t manufacturerId = 0xFFFF;
    uint32_t cod;
    uint32_t codMajorDevice;
    uint32_t codServices;
    UUID128* uuid;
    bool isRegistered;

    BluetoothDeviceInfo(const BluetoothDeviceInfo& other);
    BluetoothDeviceInfo(esp_bd_addr_t bda);
    ~BluetoothDeviceInfo();

    const char* getManufacturerName() const;
    const char* getAddress() const;
    bool hasAddress(esp_bd_addr_t& otherAddress) const;
    bool hasName() const;
    void setName(void* namePtr, uint8_t length);

    bool operator<(const BluetoothDeviceInfo& other) const;
};


class Bluetooth
{
    public:
        //Constructor
        Bluetooth();

        static const char* formatDeviceAddress(const esp_bd_addr_t& bda);

        void registerDevices(int deviceCount, const esp_bd_addr_t* deviceAddresses);

        virtual bool begin(const char* deviceName, int rssiLimit = -90);
        virtual bool startDiscovery(uint32_t duration = 5);

        const char* getStateName();

        BluetoothState inline getState()
        {
            return _state;
        }

        bool inline isDeviceDetected()
        {
            return _isDeviceDetected;
        }

        std::vector<BluetoothDeviceInfo> inline getDiscoveredDevices()
        {
            return _discoveredDevices;
        }

    protected:
        static Bluetooth* _instancePtr;
        static const char* _stateNames[];
        const char* _deviceName;
        BluetoothState _state = BluetoothState::Uninitialized;
        int _rssiLimit;
        int _registeredDeviceCount;
        const esp_bd_addr_t* _registeredDevices;
        bool _isDeviceDetected;
        std::vector<BluetoothDeviceInfo> _discoveredDevices;
};

#endif