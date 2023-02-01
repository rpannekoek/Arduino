#include <nvs.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <Tracer.h>
#include <PSRAM.h>

#include "BluetoothClassic.h"


// Constructor
BluetoothClassic::BluetoothClassic(esp_bt_connection_mode_t connectMode, esp_bt_discovery_mode_t discoveryMode)
{
    _connectMode = connectMode;
    _discoveryMode = discoveryMode;
}


bool BluetoothClassic::begin(const char* deviceName, int rssiLimit)
{
    Tracer tracer(F("BluetoothClassic::begin"), deviceName);

    if (!btStarted())
    {
        if (!startBluetooth())
        {
            TRACE(F("Unable to start Bluetooth\n"));
            return false;
        }
    }

    esp_err_t err;
    esp_bluedroid_status_t blueDroidStatus = esp_bluedroid_get_status();
    if (blueDroidStatus == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        err = esp_bluedroid_init();
        if (err != ESP_OK)
        {
            TRACE(F("esp_bluedroid_init() returned %X\n"), err);
            return false;
        }
        blueDroidStatus = ESP_BLUEDROID_STATUS_INITIALIZED;
    }
    if (blueDroidStatus == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        err = esp_bluedroid_enable();
        if (err != ESP_OK)
        {
            TRACE(F("esp_bluedroid_enable() returned %X\n"), err);
            return false;
        }
    }

    if (_pinCode == nullptr)
    {
        // Set default parameters for Secure Simple Pairing
        esp_bt_io_cap_t ioCapability = ESP_BT_IO_CAP_IO;
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &ioCapability, sizeof(uint8_t));
    }
    else
    {
        // Set default parameters for Legacy Pairing
        esp_bt_pin_code_t pinBuffer;
        strncpy((char*)pinBuffer, _pinCode, sizeof(pinBuffer));
        uint8_t pinLength = std::min(strlen(_pinCode), sizeof(pinBuffer));
        esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, pinLength, pinBuffer);
    }

    err = esp_bt_dev_set_device_name(deviceName);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_dev_set_device_name('%s') returned %X\n"), deviceName, err);
        return false;
    }

    err = esp_bt_gap_register_callback(bt_gap_cb);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_register_callback() returned %X\n"), err);
        return false;
    }

    err = esp_bt_gap_set_scan_mode(_connectMode, _discoveryMode);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_set_scan_mode() returned %X\n"), err);
        return false;
    }

    Bluetooth::begin(deviceName, rssiLimit);

    return true;
}


bool BluetoothClassic::startBluetooth()
{
    Tracer tracer(F("BluetoothClassic::startBluetooth"));

    // return btStart(); // Defined in esp32-hal-bt.c, but always starts dual mode (Classic+BLE)

    esp_err_t err;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        esp_bt_controller_config_t btConfig = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        btConfig.mode = ESP_BT_MODE_CLASSIC_BT;
        err = esp_bt_controller_init(&btConfig);
        if (err != ESP_OK)
        {
            TRACE(F("esp_bt_controller_init() returned %X\n"), err);
            return false;
        }

        int timeout = 0;
        while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE)
        {
            delay(10);
            if (timeout++ == 100)
            {
                TRACE(F("Timeout waiting for Bluetooth initialization.\n"));
                return false;
            }
        }
    }
    
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT); 
        if (err != ESP_OK) 
        {
            TRACE(F("esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) returned %X\n"), err);
            return false;
        }
    }

    return true;
}


bool BluetoothClassic::startDiscovery(uint32_t duration)
{
    Tracer tracer(F("BluetoothClassic::startDiscovery"));

    duration = std::min(duration, 48U);

    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, duration, 0);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_start_discovery() returned %X\n"), err);
        return false;
    }

    Bluetooth::startDiscovery(duration);
    return true;
}


void BluetoothClassic::addDiscoveredDevice(esp_bt_gap_cb_param_t* gapParam)
{
    esp_bd_addr_t& bda = gapParam->disc_res.bda;
    const char* deviceAddress = formatDeviceAddress(bda);
    Tracer tracer(F("BluetoothClassic::addDiscoveredDevice"), deviceAddress);

    // Check if the device was already added earlier
    for (BluetoothDeviceInfo& btDeviceInfo : _discoveredDevices)
    {
        if (btDeviceInfo.hasAddress(bda))
        {
            TRACE(F("Device already added.\n"));
            return;
        } 
    }

    BluetoothDeviceInfo btDeviceInfo(bda);
    uint8_t* eirDataPtr = nullptr;
    int eirLength = 0;

    for (int i = 0; i < gapParam->disc_res.num_prop; i++)
    {
        esp_bt_gap_dev_prop_t& gapDevProp = gapParam->disc_res.prop[i]; 
        switch (gapDevProp.type) 
        {
            case ESP_BT_GAP_DEV_PROP_COD:
                btDeviceInfo.cod = *((uint32_t*)gapDevProp.val);
                break;

            case ESP_BT_GAP_DEV_PROP_RSSI:
                btDeviceInfo.rssi = *((int8_t*)gapDevProp.val);
                break;

            case ESP_BT_GAP_DEV_PROP_BDNAME:
                btDeviceInfo.setName(gapDevProp.val, gapDevProp.len);
                break;

            case ESP_BT_GAP_DEV_PROP_EIR:
                eirLength = gapDevProp.len;
                eirDataPtr = (uint8_t*) ESP_MALLOC(gapDevProp.len);
                memcpy(eirDataPtr, gapDevProp.val, gapDevProp.len);
                break;

            default:
                TRACE(F("Unexpected GAP property type: %d\n"), gapDevProp.type);
        }
    }

    btDeviceInfo.codMajorDevice = esp_bt_gap_get_cod_major_dev(btDeviceInfo.cod); 
    btDeviceInfo.codServices = esp_bt_gap_get_cod_srvc(btDeviceInfo.cod);

    if (!btDeviceInfo.hasName())
    {
        // Try to get device name from EIR
        if (eirDataPtr != nullptr)
        {
            uint8_t length;
            uint8_t* eirPtr = esp_bt_gap_resolve_eir_data(eirDataPtr, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
            if (eirPtr == nullptr)
                eirPtr = esp_bt_gap_resolve_eir_data(eirDataPtr, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
            if (eirPtr != nullptr)
                btDeviceInfo.setName(eirPtr, length);
            else
                TRACE(F("No device name in EIR\n"));
        }
        else
            TRACE(F("No EIR data\n"));

        if (!btDeviceInfo.hasName())
        {
            TRACE(F("No device name found, using address.\n"));
            btDeviceInfo.setName((void*)deviceAddress, strlen(deviceAddress));
        }
    }

    _discoveredDevices.push_back(btDeviceInfo);
    TRACE(F("%d discovered devices\n"), _discoveredDevices.size());

    for (int i = 0; i < _registeredDeviceCount; i++)
    {
        if (memcmp(bda, _registeredDevices[i], sizeof(esp_bd_addr_t)) == 0)
        {
            _isDeviceDetected = true;
            break;
        }
    }

    if (eirDataPtr != nullptr) free(eirDataPtr);
}


void BluetoothClassic::bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    static_cast<BluetoothClassic*>(Bluetooth::_instancePtr)->gapCallback(event, param);
}


void BluetoothClassic::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    TRACE(F("BluetoothClassic::gapCallback(%d)  [Core #%d]\n"), event, xPortGetCoreID());

    switch (event) 
    {
        case ESP_BT_GAP_DISC_RES_EVT:
            addDiscoveredDevice(param);
            break;

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED)
                _state = BluetoothState::Discovering;
            else
                _state = BluetoothState::DiscoveryComplete;
            break;

        case ESP_BT_GAP_AUTH_CMPL_EVT: 
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) 
            {
                _remoteDeviceName = (const char*)param->auth_cmpl.device_name;
                _state = BluetoothState::Authenticated;
                TRACE(F("Authentication success. Remote device name: '%s'\n"), _remoteDeviceName.c_str());
            } else 
            {
                _state = BluetoothState::AuthenticationFailed;
                TRACE(F("Authentication failed. Status: %d\n"), param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            // TODO: invoke custom handler
            TRACE(F("Confirmation request for code '%d'\n"), param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            // TODO
            TRACE(F("ESP_BT_GAP_KEY_NOTIF_EVT. Passkey: '%d'\n"), param->key_notif.passkey);
            break;

        case ESP_BT_GAP_KEY_REQ_EVT:
            // TODO
            TRACE(F("ESP_BT_GAP_KEY_REQ_EVT. Please enter passkey.\n"));
            break;
    }
}
