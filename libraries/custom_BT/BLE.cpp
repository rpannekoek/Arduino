#include <BLEDevice.h>
#include <Tracer.h>
#include "BLE.h"


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
    TRACE(F("BLE scan complete. Found %d devices:\n"), scanResults.getCount());

    _discoveredDevices.clear();
    for (int i = 0; i < scanResults.getCount(); i++)
    {
        BLEAdvertisedDevice bleDevice = scanResults.getDevice(i);
        TRACE(F("\t%s (RSSI=%d)\n"), bleDevice.toString().c_str(), bleDevice.getRSSI());

        if (bleDevice.getRSSI() >= _rssiLimit)
        {
            BluetoothDeviceInfo btDevice(*bleDevice.getAddress().getNative());
            if (bleDevice.getName().length() != 0)
                btDevice.setName((void*)bleDevice.getName().c_str(), bleDevice.getName().length());
            btDevice.rssi = bleDevice.getRSSI();

            if (bleDevice.haveManufacturerData())
            {
                uint8_t* manufacturerData = (uint8_t*)bleDevice.getManufacturerData().data();
                btDevice.manufacturerId = manufacturerData[0] | (manufacturerData[1] << 8);;
                TRACE(F("\tManufacturer: %s\n"), btDevice.getManufacturerName());   
            }

            TRACE(F("\tAddress type: %d\n\n"), bleDevice.getAddressType());

            _discoveredDevices.push_back(btDevice);
        }
    }

    std::sort(_discoveredDevices.begin(), _discoveredDevices.end());

    _state = BluetoothState::DiscoveryComplete; 
}


void BLE::onResult(BLEAdvertisedDevice advertisedDevice)
{
    int rssi = advertisedDevice.getRSSI();
    if (rssi < _rssiLimit) return;

    esp_bd_addr_t* bdaPtr = advertisedDevice.getAddress().getNative();
    for (int i = 0; i < _registeredDeviceCount; i++)
    {
        if (memcmp(bdaPtr, _registeredDevices[i], sizeof(esp_bd_addr_t)) == 0)
        {
            TRACE(F("Registered device detected.\n"));
            _isDeviceDetected = true;
        }
    }
}
