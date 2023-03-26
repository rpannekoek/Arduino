#include <BLEDevice.h>
#include <Tracer.h>
#include "BLE.h"


void BLE::registerBeacons(int count, const uuid128_t* uuids)
{
    _registeredBeaconCount = count;
    _registeredBeacons = uuids;
}


bool BLE::begin(const char* deviceName, int rssiLimit)
{
    Tracer tracer(F("BLE::begin"), deviceName);

    BLEDevice::init(deviceName);
    
    Bluetooth::begin(deviceName, rssiLimit);
    
    return true; // TODO
}


bool BLE::startDiscovery(uint32_t duration)
{
    Tracer tracer(F("BLE::startDiscovery"));

    Bluetooth::startDiscovery(duration);

    BLEScan* bleScanPtr = BLEDevice::getScan();
    bleScanPtr->setAdvertisedDeviceCallbacks(this);
    bleScanPtr->setActiveScan(true); //active scan uses more power, but get results faster
    bleScanPtr->setInterval(_scanIntervalMs);
    bleScanPtr->setWindow(_scanWindowMs);

    return bleScanPtr->start(duration, _scanComplete);
}


void BLE::_scanComplete(BLEScanResults scanResults)
{
    static_cast<BLE*>(Bluetooth::_instancePtr)->scanComplete(scanResults); 
}        


void BLE::scanComplete(BLEScanResults& scanResults)
{
    TRACE(F("BLE scan complete. Found %d devices.\n"), _discoveredDevices.size());

    // TODO: This does weird stuff with BluetoothDeviceInfo copy constructors
    //std::sort(_discoveredDevices.begin(), _discoveredDevices.end());

    _state = BluetoothState::DiscoveryComplete; 
}


void BLE::onResult(BLEAdvertisedDevice bleDevice)
{
    TRACE(F("Advertised Device: %s\n"), bleDevice.toString().c_str());

    if (bleDevice.getRSSI() < _rssiLimit) return;

    BluetoothDeviceInfo btDevice(*bleDevice.getAddress().getNative());
    btDevice.rssi = bleDevice.getRSSI();

    if (bleDevice.haveName())
        btDevice.setName((void*)bleDevice.getName().c_str(), bleDevice.getName().length());

    if (bleDevice.haveManufacturerData())
    {
        std::string manufacturerData = bleDevice.getManufacturerData();
        btDevice.manufacturerId = manufacturerData[0] | (manufacturerData[1] << 8);;
        TRACE(F("\tManufacturer: %s\n"), btDevice.getManufacturerName());

        if ((btDevice.manufacturerId == 0x4C) && (manufacturerData[2] == 0x02))
        {
            _bleBeacon.setData(manufacturerData);

            strcpy(btDevice.name, "iBeacon");
            std::string uuid = _bleBeacon.getProximityUUID().toString();
            btDevice.uuid = new UUID128(uuid.c_str());
            TRACE(F("\tiBeacon: %s\n"), uuid.c_str());

            for (int i = 0; i < _registeredBeaconCount; i++)
            {
                if (btDevice.uuid->equals(_registeredBeacons[i]))
                {
                    TRACE(F("Registered beacon detected.\n"));
                    btDevice.isRegistered = true;
                    _isDeviceDetected = true;
                    break;
                }
            }
        }
    }

    esp_bd_addr_t* bdaPtr = bleDevice.getAddress().getNative();
    for (int i = 0; i < _registeredDeviceCount; i++)
    {
        if (memcmp(bdaPtr, _registeredDevices[i], sizeof(esp_bd_addr_t)) == 0)
        {
            TRACE(F("Registered device detected.\n"));
            btDevice.isRegistered = true;
            _isDeviceDetected = true;
            break;
        }
    }

    _discoveredDevices.push_back(btDevice);
}
