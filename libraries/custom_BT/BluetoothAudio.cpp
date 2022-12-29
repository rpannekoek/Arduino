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

#include "BluetoothAudio.h"

static const char* _stateNames[] = {
    "Uninitialized",
    "Initialized",
    "Discovering",
    "Discovery Complete",
    "Awaiting Connection",
    "Authenticated",
    "Authentication Failed",
    "Awaiting Source",
    "Source Not Ready",
    "Audio Connecting",
    "Audio Connected",
    "Audio Started",
    "Audio Suspended",
    "Audio Stopped",
    "Audio Disconnecting",
    "Audio Disconnected"
    };

static const char* _a2dpConnectionState[] = { "Disconnected", "Connecting", "Connected", "Disconnecting" };
static const char* _a2dpAudioState[] = { "Suspended", "Stopped", "Started" };
static const char* _mediaControlCommands[] = { "None", "Check Source Ready", "Start", "Stop", "Suspend" };

BluetoothAudio* _instancePtr = nullptr;

// Global GAP callback function
void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    _instancePtr->gapCallback(event, param);
}

// Global A2DP callback function
void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    _instancePtr->a2dpCallback(event, param);
}


String getDeviceName(void* namePtr, uint8_t length)
{
    static char strBuf[ESP_BT_GAP_MAX_BDNAME_LEN];
    memcpy(strBuf, namePtr, length);
    strBuf[length] = 0;
    return String(strBuf);
}


// Constructor
BluetoothAudio::BluetoothAudio()
    : discoveredDevices(8)
{
    _instancePtr = this;
}


String BluetoothAudio::getStateName()
{
    uint16_t stateIndex = static_cast<uint16_t>(_state);
    if (stateIndex <= 15) 
        return _stateNames[stateIndex];
    else
        return String(stateIndex);
}


const char* BluetoothAudio::formatDeviceAddress(esp_bd_addr_t bda)
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


bool BluetoothAudio::begin(const char* deviceName, const char* pinCode)
{
    Tracer tracer(F("BluetoothAudio::begin"), deviceName);

    _deviceName = deviceName;
    _pinCode = pinCode;

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

    err = esp_a2d_register_callback(bt_a2d_cb);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_register_callback() returned %X\n"), err);
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


bool BluetoothAudio::startDiscovery(uint8_t inquiryTime)
{
    Tracer tracer(F("BluetoothAudio::startDiscovery"));

    if (_sinkStarted || _sourceStarted)
    {
        TRACE(F("Cannot start discovery if Sink or Source is started.\n"));
        return false;
    }

    if (inquiryTime > 48) inquiryTime = 48;

    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, inquiryTime, 0);
    if (err != ESP_OK)
    {
        TRACE(F("esp_bt_gap_start_discovery() returned %X\n"), err);
        return false;
    }

    _state = BluetoothState::Discovering;
    discoveredDevices.clear();
    return true;
}


void BluetoothAudio::addDiscoveredDevice(esp_bt_gap_cb_param_t* gapParam)
{
    esp_bd_addr_t& bda = gapParam->disc_res.bda;
    TRACE(F("BluetoothAudio::addDiscoveredDevice(%s)\n"), formatDeviceAddress(bda));

    // Check if the device was already added earlier
    BluetoothDeviceInfo* devInfoPtr = discoveredDevices.getFirstEntry();
    while (devInfoPtr != nullptr)
    {
        if (memcmp(devInfoPtr->deviceAddress, bda, sizeof(esp_bd_addr_t)) == 0)
            return;
        devInfoPtr = discoveredDevices.getNextEntry();
    }

    devInfoPtr = new BluetoothDeviceInfo();
    memcpy(devInfoPtr->deviceAddress, bda, sizeof(esp_bd_addr_t));
    devInfoPtr->deviceName = String();

    for (int i = 0; i < gapParam->disc_res.num_prop; i++)
    {
        esp_bt_gap_dev_prop_t& gapDevProp = gapParam->disc_res.prop[i]; 
        switch (gapDevProp.type) 
        {
            case ESP_BT_GAP_DEV_PROP_COD:
                devInfoPtr->cod = *((uint32_t*)gapDevProp.val);
                break;

            case ESP_BT_GAP_DEV_PROP_RSSI:
                devInfoPtr->rssi = *((int8_t*)gapDevProp.val);
                break;

            case ESP_BT_GAP_DEV_PROP_BDNAME:
                devInfoPtr->deviceName = ::getDeviceName(gapDevProp.val, gapDevProp.len);
                break;

            case ESP_BT_GAP_DEV_PROP_EIR:
                devInfoPtr->eirLength = gapDevProp.len;
                devInfoPtr->eirData = (uint8_t*) ESP_MALLOC(gapDevProp.len);
                memcpy(devInfoPtr->eirData, gapDevProp.val, gapDevProp.len);
                break;

            default:
                TRACE(F("Unexpected GAP property type: %d\n"), gapDevProp.type);
        }
    }

    devInfoPtr->codMajorDevice = esp_bt_gap_get_cod_major_dev(devInfoPtr->cod); 
    devInfoPtr->codServices = esp_bt_gap_get_cod_srvc(devInfoPtr->cod);

    if (devInfoPtr->deviceName.length() == 0)
    {
        // Try to get device name from EIR
        uint8_t* eir = devInfoPtr->eirData;
        if (eir != nullptr)
        {
            uint8_t length;
            uint8_t* eirPtr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
            if (eirPtr == nullptr)
                eirPtr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
            if (eirPtr != nullptr)
                devInfoPtr->deviceName = ::getDeviceName(eirPtr, length);
        }

        if (devInfoPtr->deviceName.length() == 0)
        {
            TRACE(F("No device name found\n"));
            devInfoPtr->deviceName = formatDeviceAddress(bda);
        }
    }

    discoveredDevices.add(devInfoPtr);
    TRACE(F("%d discovered devices\n"), discoveredDevices.count());
}


bool BluetoothAudio::startSink(esp_a2d_sink_data_cb_t dataCallback)
{
    Tracer tracer(F("BluetoothAudio::startSink"));

    if (_sourceStarted)
    {
        TRACE(F("Cannot start Sink because Source is started already.\n"));
        return false;
    }

    esp_err_t err = esp_a2d_sink_init();
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_sink_init() returned %X\n"), err);
        return false;
    }

    err= esp_a2d_sink_register_data_callback(dataCallback);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_sink_register_data_callback() returned %X\n"), err);
        return false;
    }

    _sinkStarted = true;
    _state = BluetoothState::AwaitingConnection;
    return true;
}


bool BluetoothAudio::awaitAudioDisconnect()
{
    Tracer tracer(F("BluetoothAudio::awaitAudioDisconnect"));

    int timeout = 50;
    while (_state != BluetoothState::AudioDisconnected)
    {
        if (timeout-- == 0)
        {
            TRACE(F("Timeout waiting for audio disconnect.\n"));
            _state = BluetoothState::AudioDisconnected;
            _remoteDevice = String();
            esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
            return false;
        }
        delay(10);
    }

    return true;
}


bool BluetoothAudio::stopSink()
{
    Tracer tracer(F("BluetoothAudio::stopSink"));

    if (!_sinkStarted)
    {
        TRACE(F("Sink was not started.\n"));
        return false;
    }

    esp_err_t err;
    switch (_state)
    {
        case BluetoothState::AwaitingConnection:
            _state = BluetoothState::Initialized;
            break;

        case BluetoothState::AudioConnected:
        case BluetoothState::AudioStarted:
        case BluetoothState::AudioSuspended:
        case BluetoothState::AudioStopped:
            err = esp_a2d_sink_disconnect(_remoteDeviceAddress);
            if (err != ESP_OK)
            {
                TRACE(F("esp_a2d_sink_disconnect() returned %X\n"), err);
                return false;
            }
            awaitAudioDisconnect();
    }

    err = esp_a2d_sink_deinit();
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_sink_deinit() returned %X\n"), err);
        return false;
    }

    _sinkStarted = false;
    return true;
}


bool BluetoothAudio::connectSource(esp_bd_addr_t sinkAddress, esp_a2d_source_data_cb_t dataCallback)
{
    Tracer tracer(F("BluetoothAudio::connectSource"));

    if (_sinkStarted)
    {
        TRACE(F("Cannot start Source because Sink is started already.\n"));
        return false;
    }

    esp_err_t err = esp_a2d_source_init();
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_source_init() returned %X\n"), err);
        return false;
    }

    err = esp_a2d_source_register_data_callback(dataCallback);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_source_register_data_callback() returned %X\n"), err);
        return false;
    }

    err = esp_a2d_source_connect(sinkAddress);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_source_connect() returned %X\n"), err);
        return false;
    }

    memcpy(_remoteDeviceAddress, sinkAddress, sizeof(esp_bd_addr_t));

    _sampleRate = 44100; // Currently hard-coded in ESP IDF
    _sourceStarted = true;
    _state = BluetoothState::AudioConnecting;
    return true;
}


bool BluetoothAudio::disconnectSource()
{
    Tracer tracer(F("BluetoothAudio::disconnectSource"));

    if (!_sourceStarted)
    {
        TRACE(F("Source was not connected.\n"));
        return false;
    }

    esp_err_t err = esp_a2d_source_disconnect(_remoteDeviceAddress);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_source_disconnect() returned %X\n"), err);
        return false;
    }

    awaitAudioDisconnect();

    err = esp_a2d_source_deinit();
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_source_deinit() returned %X\n"), err);
        return false;
    }

    _sourceStarted = false;
    return true;
}


bool BluetoothAudio::mediaControl(esp_a2d_media_ctrl_t ctrl)
{
    TRACE(F("BluetoothAudio::mediaControl('%s') [Core #%d]\n"), _mediaControlCommands[ctrl], xPortGetCoreID());
    esp_err_t err = esp_a2d_media_ctrl(ctrl);
    if (err != ESP_OK)
    {
        TRACE(F("esp_a2d_media_ctrl(%d) returned %X\n"), ctrl, err);
        return false;
    }
    return true;
}


void BluetoothAudio::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    TRACE(F("BluetoothAudio::gapCallback(%d)  [Core #%d]\n"), event, xPortGetCoreID());

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
                _remoteDevice = (const char*)param->auth_cmpl.device_name;
                _state = BluetoothState::Authenticated;
                TRACE(F("Authentication success. Remote device name: '%s'\n"), _remoteDevice.c_str());
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
    TRACE(F("BluetoothAudio::a2dpCallback(%d) [Core #%d]\n"), event, xPortGetCoreID());

    switch (event) 
    {
        case ESP_A2D_CONNECTION_STATE_EVT:
        {
            memcpy(_remoteDeviceAddress, param->conn_stat.remote_bda, sizeof(_remoteDeviceAddress));
            const char* remoteDeviceAddress = formatDeviceAddress(_remoteDeviceAddress);
            TRACE(F("A2DP Connection state change: %s. Remote device address: [%s]\n"),
                _a2dpConnectionState[param->conn_stat.state], remoteDeviceAddress);
            switch (param->conn_stat.state)
            {
                case ESP_A2D_CONNECTION_STATE_CONNECTING:
                    _state = BluetoothState::AudioConnecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_CONNECTED:
                    if (_remoteDevice.length() == 0)
                        _remoteDevice = remoteDeviceAddress;
                    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_NONE);
                    if (_sourceStarted)
                    {
                        _state = BluetoothState::AwaitingSource;
                        mediaControl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
                    }
                    else
                        _state = BluetoothState::AudioConnected;
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
                    _state = BluetoothState::AudioDisconnecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
                    _state = BluetoothState::AudioDisconnected;
                    _remoteDevice = String();
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

        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            TRACE(
                F("Media control '%s' result: %d\n"),
                _mediaControlCommands[param->media_ctrl_stat.cmd],
                param->media_ctrl_stat.status
                );
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY)
            {
                if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS)
                    _state = BluetoothState::AudioConnected;
                else
                    _state = BluetoothState::SourceNotReady;
            }
            break;            
    }
}
