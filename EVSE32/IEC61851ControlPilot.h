#include <driver/adc.h>
#include <Ticker.h>


enum struct ControlPilotStatus
{
    Standby = 0,
    VehicleDetected = 1,
    Charging = 2,
    ChargingVentilated = 3,
    NoPower = 4
};


class IEC61851ControlPilot
{
    public:
        IEC61851ControlPilot(uint8_t outputPin, uint8_t inputPin, uint8_t feedbackPin, uint8_t pwmChannel = 0, float maxCurrent = 16);

        bool begin(float scale = 0.0041); // Approximation using 82k/22k voltage divider

        float calibrate();

        void setOff();

        void setReady();

        float setCurrentLimit(float ampere);

        float getVoltage();

        bool awaitStatus(ControlPilotStatus status, int timeoutMs = 100);

        ControlPilotStatus inline getStatus()
        {
            return _status;
        }

        const char* getStatusName();

        float inline getDutyCycle()
        {
            return _dutyCycle;
        }

    private:
        uint8_t _outputPin;
        uint8_t _inputPin;
        uint8_t _feedbackPin;
        uint8_t _pwmChannel;
        adc1_channel_t _adcChannel;
        float _dutyCycle;
        float _scale;
        float _maxCurrent;
        ControlPilotStatus volatile _status;
        Ticker _statusTicker;

        void determineStatus(); 
        static void determineStatus(IEC61851ControlPilot* instancePtr); 
};