#include <Arduino.h>
#include <Bluetooth.h>
#include <map>

const char* Bluetooth::_stateNames[] =
{
    "Uninitialized",
    "Initialized",
    "Discovering",
    "Discovery Complete",
    "Authenticated",
    "Authentication Failed"
};

std::map<uint16_t, const char*> _knownManufacturers = 
{
    { 0, "Ericsson" },
    { 6, "Microsoft" },
    { 0x4C, "Apple" },
    { 0X75, "Samsung" },
    { 0X87, "Garmin" },
    { 0xE0, "Google" }
};


Bluetooth* Bluetooth::_instancePtr = nullptr;


// Constructor
Bluetooth::Bluetooth()
{
    _instancePtr = this;
}


const char* Bluetooth::getStateName()
{
    uint16_t stateIndex = static_cast<uint16_t>(_state);
    if (stateIndex <= 5) 
        return _stateNames[stateIndex];
    else
        return "(Unknown)";
}


const char* Bluetooth::formatDeviceAddress(const esp_bd_addr_t& bda)
{
    static char result[32];
    snprintf(
        result,
        sizeof(result),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]
        );
    return result;
}


void Bluetooth::registerDevices(int deviceCount, const esp_bd_addr_t* deviceAddresses)
{
    _registeredDeviceCount = deviceCount;
    _registeredDevices = deviceAddresses;
}


bool Bluetooth::begin(const char* deviceName, int rssiLimit)
{
    _deviceName = deviceName;
    _rssiLimit = rssiLimit;
    _isDeviceDetected = false;
    _state = BluetoothState::Initialized;
    return false; // Subclass should override this method
}


bool Bluetooth::startDiscovery(uint32_t duration)
{
    _discoveredDevices.clear();
    _isDeviceDetected = false;
    _state = BluetoothState::Discovering;
    return false; // Subclass should override this method
}


BluetoothDeviceInfo::BluetoothDeviceInfo(esp_bd_addr_t bda)
{
    memcpy(address, bda, sizeof(esp_bd_addr_t));
    name[0] = 0;
}


const char* BluetoothDeviceInfo::getManufacturerName() const
{
    if (_knownManufacturers.count(manufacturerId) == 0)
    {
        static char result[8];
        snprintf(result, sizeof(result), "0x%04X", manufacturerId);
        return result;
    }
    else
        return  _knownManufacturers[manufacturerId];
}


const char* BluetoothDeviceInfo::getAddress() const
{
    return Bluetooth::formatDeviceAddress(address);        
}


bool BluetoothDeviceInfo::hasAddress(esp_bd_addr_t& otherAddress) const
{
    return memcmp(address, otherAddress, sizeof(esp_bd_addr_t)) == 0;
}


bool BluetoothDeviceInfo::hasName() const
{
    return name[0] != 0;
}


void BluetoothDeviceInfo::setName(void* namePtr, uint8_t length)
{
    length = std::min(length, (uint8_t)(sizeof(name) - 1));
    memcpy(name, namePtr, length);
    name[length] = 0;
}


bool BluetoothDeviceInfo::operator<(const BluetoothDeviceInfo& other) const
{
    return rssi > other.rssi;
}

