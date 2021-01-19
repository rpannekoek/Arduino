#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
    #error Bluetooth is not enabled!
#endif

enum struct BluetoothState
{
    Uninitialized = 0,
    Initialized,
    Authenticated,
    AuthenticationFailed,
    AudioConnecting,
    AudioConnected,
    AudioStarted,
    AudioSuspended,
    AudioStopped,
    AudioDisconnecting,
    AudioDisconnected
};

class BluetoothAudio
{

    public:
        // Constructor
        BluetoothAudio();

        BluetoothState inline getState()
        {
            return _state;
        }

        String inline getRemoteDeviceName()
        {
            return _remoteDeviceName;
        }

        esp_a2d_mct_t inline getCodecType()
        {
            return _codecType;
        }

        uint16_t inline getSampleRate()
        {
            return _sampleRate;
        }

        uint32_t inline getPacketsReceived()
        {
            return _packetsReceived;
        }

        bool begin(const char* deviceName, const char* pinCode = nullptr);
        bool startSink(void (*dataHandler)(const uint8_t* data, uint32_t length));

    protected:
        const char* _deviceName;
        const char* _pinCode;
        BluetoothState _state = BluetoothState::Uninitialized;
        String _remoteDeviceName;
        esp_bd_addr_t _remoteDeviceAddress;
        esp_a2d_mct_t _codecType = 0;
        uint16_t _sampleRate = 0;
        uint32_t _packetsReceived = 0;
        void (*_sinkDataHandler)(const uint8_t* data, uint32_t length);

        bool startBluetooth();
        void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
        void a2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
        void a2dpDataSinkCallback(const uint8_t* data, uint32_t length);

    private:
        friend void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
        friend void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
        friend void bt_a2d_data_cb(const uint8_t* data, uint32_t length);
};

#endif