#include <esp32_digital_led_lib.h>
#include <Ticker.h>

enum EVSEState
{
    Booting = 0,
    SelfTest = 1,
    Failure = 2,
    Ready = 3,
    Authorize = 4,
    AwaitCharging = 5,
    Charging = 6,
    ChargeCompleted = 7
};

extern const char* EVSEStateNames[];

class StatusLED
{
    public:

        StatusLED(int8_t pin);

        bool begin();

        bool setColor(pixelColor_t color);
        bool setStatus(EVSEState status);

    private:
        static pixelColor_t _statusColors[];
        static float _breatheTable[];
        Ticker _breatheTicker;
        int _breatheIndex;
        strand_t _ledStrand;
        pixelColor_t _statusColor;

        static void breathe(StatusLED* instancePtr);
        void breathe();

};