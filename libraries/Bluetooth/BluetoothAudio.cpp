#include <nvs.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <Tracer.h>

#include "BluetoothAudio.h"

static const char* _a2dpConnectionState[] = { "Disconnected", "Connecting", "Connected", "Disconnecting" };
static const char* _a2dpAudioState[] = { "Suspended", "Stopped", "Started" };
BluetoothAudio* _instancePtr = nullptr;

// Global callback function
void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    _instancePtr->gapCallback(event, param);
}

// Global callback function
void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    _instancePtr->a2dpCallback(event, param);
}

// Global callback function
void bt_a2d_data_cb(const uint8_t* data, uint32_t length)
{
    _instancePtr->a2dpDataSinkCallback(data, length);
}


// Constructor
BluetoothAudio::BluetoothAudio()
{
    _instancePtr = this;
}


bool BluetoothAudio::begin(const char* deviceName, const char* pinCode)
{
    Tracer tracer(F("BluetoothAudio::begin"), deviceName);

    _deviceName = deviceName;
    _pinCode = pinCode;

    /* NVS initialization is done by Arduino Core (esp32-hal-misc.c:initArduino())
    // Initialize NVS — it is used to store PHY calibration data
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            TRACE(F("nvs_flash_erase() returned %X\n"), err);
            return false;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        TRACE(F("nvs_flash_init() returned %X\n"), err);
        return false;
    }
    */

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

    err = esp_bt_dev_set_device_name(_deviceName);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_dev_set_device_name('%s') returned %X\n"), _deviceName, err);
        return false;
    }

    err = esp_bt_gap_register_callback(bt_gap_cb);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_register_callback() returned %X\n"), err);
        return false;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_set_scan_mode() returned %X\n"), err);
        return false;
    }

    _state = BluetoothState::Initialized;
    return true;
}

bool BluetoothAudio::startBluetooth()
{
    Tracer tracer(F("BluetoothAudio::startBluetooth"));

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


bool BluetoothAudio::startSink(void (*dataHandler)(const uint8_t* data, uint32_t length))
{
    if (_state != BluetoothState::Initialized)
    {
        TRACE(F("Invalid state: %d\n"), _state);
        return false;
    }

    _sinkDataHandler = dataHandler;

    esp_err_t err = esp_a2d_register_callback(bt_a2d_cb);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_register_callback() returned %X\n"), err);
        return false;
    }

    err= esp_a2d_sink_register_data_callback(bt_a2d_data_cb);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_sink_register_data_callback() returned %X\n"), err);
        return false;
    }

    err = esp_a2d_sink_init();
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_sink_init() returned %X\n"), err);
        return false;
    }

    return true;
}


void BluetoothAudio::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    TRACE(F("BluetoothAudio::gapCallback(%d)\n"), event);

    switch (event) 
    {
        case ESP_BT_GAP_AUTH_CMPL_EVT: 
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) 
            {
                _remoteDeviceName = (const char*)param->auth_cmpl.device_name;
                _state = BluetoothState::Authenticated;
                TRACE(F("Authentication success. Remote device: %s\n"), _remoteDeviceName.c_str());
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


void BluetoothAudio::a2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    TRACE(F("BluetoothAudio::a2dpCallback(%d)\n"), event);

    switch (event) 
    {
        case ESP_A2D_CONNECTION_STATE_EVT:
        {
            memcpy(_remoteDeviceAddress, param->conn_stat.remote_bda, sizeof(_remoteDeviceAddress));
            uint8_t* bda = _remoteDeviceAddress;
            TRACE(F("A2DP Connection state change: %s. Remote address: [%02X:%02X:%02X:%02X:%02X:%02X]\n"),
                _a2dpConnectionState[param->conn_stat.state], bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            switch (param->conn_stat.state)
            {
                case ESP_A2D_CONNECTION_STATE_CONNECTING:
                    _state = BluetoothState::AudioConnecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_CONNECTED:
                    _state = BluetoothState::AudioConnected;
                    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE);
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
                    _state = BluetoothState::AudioDisconnecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
                    _state = BluetoothState::AudioDisconnected;
                    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
                    break;
            }
            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT:
            TRACE(F("A2DP Audio state change: %s\n"), _a2dpAudioState[param->audio_stat.state]);
            switch (param->audio_stat.state)
            {
                case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
                    _state = BluetoothState::AudioSuspended;
                    break;

                case ESP_A2D_AUDIO_STATE_STOPPED:
                    _state = BluetoothState::AudioStopped;
                    break;

                case ESP_A2D_AUDIO_STATE_STARTED:
                    _state = BluetoothState::AudioStarted;
                    _packetsReceived = 0;
                    break;
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
            _codecType = param->audio_cfg.mcc.type; 
            TRACE(F("A2DP audio stream configuration. Codec type %d.\n"), _codecType);
            // for now only SBC stream is supported
            if (_codecType == ESP_A2D_MCT_SBC) 
            {
                char oct0 = param->audio_cfg.mcc.cie.sbc[0];
                if (oct0 & (0x01 << 6)) 
                    _sampleRate = 32000;
                else if (oct0 & (0x01 << 5))
                    _sampleRate = 44100;
                else if (oct0 & (0x01 << 4))
                    _sampleRate = 48000;
                else
                    _sampleRate = 16000;

                TRACE(
                    F("Configure SBC: %02X %02X %02X %02X. Sample rate=%d Hz\n"),
                    param->audio_cfg.mcc.cie.sbc[0],
                    param->audio_cfg.mcc.cie.sbc[1],
                    param->audio_cfg.mcc.cie.sbc[2],
                    param->audio_cfg.mcc.cie.sbc[3],
                    _sampleRate
                    );
            }
            break;
    }
}


void BluetoothAudio::a2dpDataSinkCallback(const uint8_t* data, uint32_t length)
{
    if (++_packetsReceived % 100 == 1)
        TRACE(F("%d A2DP packets received. Length: %d\n"), _packetsReceived, length);
    
    if (_instancePtr->_sinkDataHandler != nullptr)
        _instancePtr->_sinkDataHandler(data, length);
}
