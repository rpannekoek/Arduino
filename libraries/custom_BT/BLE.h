#ifndef BLE_H
#define BLE_H

#include <Bluetooth.h>
#include <BLEAdvertisedDevice.h>
#include <BLEScan.h>

class BLE : public Bluetooth, BLEAdvertisedDeviceCallbacks
{
    public:
        virtual bool begin(const char* deviceName, int rssiLimit = -90);
        virtual bool startDiscovery(uint32_t duration = 5);

        void setScanTimings(uint16_t intervalMs, uint16_t windowMs)
        {
            _scanIntervalMs = intervalMs;
            _scanWindowMs = windowMs;
        }

    private:
        uint16_t _scanIntervalMs = 250;
        uint16_t _scanWindowMs = 250;

        static void _scanComplete(BLEScanResults scanResults);        
        void scanComplete(BLEScanResults& scanResults);        

	    virtual void onResult(BLEAdvertisedDevice advertisedDevice);
};

#endif