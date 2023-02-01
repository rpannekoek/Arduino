#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <Log.h>
#include <BluetoothClassic.h>

enum struct BluetoothAudioState
{
    Idle = 0,
    AwaitingConnection,
    AwaitingSource,
    SourceNotReady,
    Connecting,
    Connected,
    Started,
    Suspended,
    Stopped,
    Disconnecting,
    Disconnected
};


struct StereoData
{
    int16_t right;
    int16_t left;
} __attribute__((packed));


class BluetoothAudio : public BluetoothClassic
{
    public:
        // Constructor
        BluetoothAudio();

        BluetoothAudioState inline getAudioState()
        {
            return _audioState;
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

        const char* getAudioStateName();

        bool startSink(esp_a2d_sink_data_cb_t dataCallback);
        bool stopSink();
        bool connectSource(esp_bd_addr_t sinkAddress, esp_a2d_source_data_cb_t dataCallback);
        bool disconnectSource();
        bool mediaControl(esp_a2d_media_ctrl_t ctrl);

    protected:
        static const char* _audioStateNames[];
        volatile BluetoothAudioState _audioState = BluetoothAudioState::Idle;
        esp_a2d_mct_t _codecType = 0;
        uint16_t _sampleRate = 0;
        bool _sourceStarted = false;
        bool _sinkStarted = false;

        bool awaitAudioDisconnect();

        void a2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

        static void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
};

#endif