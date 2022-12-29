#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <Log.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
    #error Bluetooth is not enabled!
#endif

enum struct BluetoothState
{
    Uninitialized = 0,
    Initialized,
    Discovering,
    DiscoveryComplete,
    AwaitingConnection,
    Authenticated,
    AuthenticationFailed,
    AwaitingSource,
    SourceNotReady,
    AudioConnecting,
    AudioConnected,
    AudioStarted,
    AudioSuspended,
    AudioStopped,
    AudioDisconnecting,
    AudioDisconnected
};


struct StereoData
{
    int16_t right;
    int16_t left;
} __attribute__((packed));


struct BluetoothDeviceInfo
{
    esp_bd_addr_t deviceAddress;
    String deviceName;
    uint32_t cod;
    uint32_t codMajorDevice;
    uint32_t codServices;
    int8_t rssi;
    uint16_t eirLength; 
    uint8_t* eirData = nullptr;

    ~BluetoothDeviceInfo()
    {
        if (eirData != nullptr)
            free(eirData);
    }
};


class BluetoothAudio
{
    public:
        Log<BluetoothDeviceInfo> discoveredDevices;

        // Constructor
        BluetoothAudio();

        BluetoothState inline getState()
        {
            return _state;
        }

        String inline getRemoteDevice()
        {
            return _remoteDevice;
        }

        esp_a2d_mct_t inline getCodecType()
        {
            return _codecType;
        }

        uint16_t inline getSampleRate()
        {
            return _sampleRate;
        }

        bool isSinkStarted()
        {
            return _sinkStarted;
        }

        bool isSourceStarted()
        {
            return _sourceStarted;
        }

        bool inline startAudio()
        {
            return mediaControl(ESP_A2D_MEDIA_CTRL_START);
        }

        bool inline suspendAudio()
        {
            return mediaControl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        }

        bool inline stopAudio()
        {
            return mediaControl(ESP_A2D_MEDIA_CTRL_STOP);
        }

        String getStateName();
        static const char* formatDeviceAddress(esp_bd_addr_t bda);

        bool begin(const char* deviceName, const char* pinCode = nullptr);
        bool startDiscovery(uint8_t inquiryTime = 5);
        bool startSink(esp_a2d_sink_data_cb_t dataCallback);
        bool stopSink();
        bool connectSource(esp_bd_addr_t sinkAddress, esp_a2d_source_data_cb_t dataCallback);
        bool disconnectSource();
        bool mediaControl(esp_a2d_media_ctrl_t ctrl);

    protected:
        const char* _deviceName;
        const char* _pinCode;
        volatile BluetoothState _state = BluetoothState::Uninitialized;
        String _remoteDevice;
        esp_bd_addr_t _remoteDeviceAddress;
        esp_a2d_mct_t _codecType = 0;
        uint16_t _sampleRate = 0;
        bool _sourceStarted = false;
        bool _sinkStarted = false;

        bool startBluetooth();
        void addDiscoveredDevice(esp_bt_gap_cb_param_t* gapParam);
        bool awaitAudioDisconnect();

        void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
        void a2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

    private:
        friend void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
        friend void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
};

#endif