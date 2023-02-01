#ifndef BLUETOOTH_CLASSIC_H
#define BLUETOOTH_CLASSIC_H

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <Bluetooth.h>
#include <Log.h>

#if !defined(CONFIG_BLUEDROID_ENABLED)
    #error Bluedroid is not enabled!
#endif


class BluetoothClassic : public Bluetooth
{
    public:
        // Constructor
        BluetoothClassic(
            esp_bt_connection_mode_t connectMode = ESP_BT_CONNECTABLE,
            esp_bt_discovery_mode_t discoveryMode = ESP_BT_GENERAL_DISCOVERABLE);

        String inline getRemoteDeviceName()
        {
            return _remoteDeviceName;
        }

        inline esp_bd_addr_t& getRemoteDeviceAddress()
        {
            return _remoteDeviceAddress;
        }

        virtual bool begin(const char* deviceName, int rssiLimit = -90);
        virtual bool startDiscovery(uint32_t duration = 5);

    protected:
        esp_bt_connection_mode_t _connectMode;
        esp_bt_discovery_mode_t _discoveryMode;
        const char* _deviceName;
        const char* _pinCode;
        String _remoteDeviceName;
        esp_bd_addr_t _remoteDeviceAddress;

        bool startBluetooth();
        void addDiscoveredDevice(esp_bt_gap_cb_param_t* gapParam);
        void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);

    private:
        static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
};

#endif