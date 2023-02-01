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

const char* BluetoothAudio::_audioStateNames[] =
{
    "Idle",
    "Awaiting Connection",
    "Awaiting Source",
    "Source Not Ready",
    "Connecting",
    "Connected",
    "Started",
    "Suspended",
    "Stopped",
    "Disconnecting",
    "Disconnected"
};

static const char* _a2dpConnectionState[] = { "Disconnected", "Connecting", "Connected", "Disconnecting" };
static const char* _a2dpAudioState[] = { "Suspended", "Stopped", "Started" };
static const char* _mediaControlCommands[] = { "None", "Check Source Ready", "Start", "Stop", "Suspend" };


// Constructor
BluetoothAudio::BluetoothAudio()
{
}


const char* BluetoothAudio::getAudioStateName()
{
    uint16_t stateIndex = static_cast<uint16_t>(_audioState);
    if (stateIndex <= 10) 
        return _audioStateNames[stateIndex];
    else
        return "(Unknown)";
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
    _audioState = BluetoothAudioState::AwaitingConnection;
    return true;
}


bool BluetoothAudio::awaitAudioDisconnect()
{
    Tracer tracer(F("BluetoothAudio::awaitAudioDisconnect"));

    int timeout = 50;
    while (_audioState != BluetoothAudioState::Disconnected)
    {
        if (timeout-- == 0)
        {
            TRACE(F("Timeout waiting for audio disconnect.\n"));
            _audioState = BluetoothAudioState::Disconnected;
            _remoteDeviceName = String();
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
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
    switch (_audioState)
    {
        case BluetoothAudioState::AwaitingConnection:
            _audioState = BluetoothAudioState::Idle;
            break;

        case BluetoothAudioState::Connected:
        case BluetoothAudioState::Started:
        case BluetoothAudioState::Suspended:
        case BluetoothAudioState::Stopped:
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
    _audioState = BluetoothAudioState::Connecting;
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


void BluetoothAudio::bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    static_cast<BluetoothAudio*>(Bluetooth::_instancePtr)->a2dpCallback(event, param);
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
                    _audioState = BluetoothAudioState::Connecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_CONNECTED:
                    if (_remoteDeviceName.length() == 0)
                        _remoteDeviceName = remoteDeviceAddress;
                    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                    if (_sourceStarted)
                    {
                        _audioState = BluetoothAudioState::AwaitingSource;
                        mediaControl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
                    }
                    else
                        _audioState = BluetoothAudioState::Connected;
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
                    _audioState = BluetoothAudioState::Disconnecting;
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
                    _audioState = BluetoothAudioState::Disconnected;
                    _remoteDeviceName = String();
                    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                    break;
            }
            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT:
            TRACE(F("A2DP Audio state change: %s\n"), _a2dpAudioState[param->audio_stat.state]);
            switch (param->audio_stat.state)
            {
                case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
                    _audioState = BluetoothAudioState::Suspended;
                    break;

                case ESP_A2D_AUDIO_STATE_STOPPED:
                    _audioState = BluetoothAudioState::Stopped;
                    break;

                case ESP_A2D_AUDIO_STATE_STARTED:
                    _audioState = BluetoothAudioState::Started;
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
                    _audioState = BluetoothAudioState::Connected;
                else
                    _audioState = BluetoothAudioState::SourceNotReady;
            }
            break;            
    }
}
