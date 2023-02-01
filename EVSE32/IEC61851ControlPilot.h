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

        bool begin(float scale = 0.0033); // Approximation using 68k/22k voltage divider

        float calibrate();

        void setOff();

        void setReady();

        float setCurrentLimit(float ampere);

        float getVoltage();

        ControlPilotStatus inline getStatus()
        {
            return _status;
        }

        const char* getStatusName();

        float getDutyCycle()
        {
            return _dutyCycle;
        }

    private:
        uint8_t _outputPin;
        uint8_t _inputPin;
        uint8_t _feedbackPin;
        uint8_t _pwmChannel;
        float _dutyCycle;
        float _scale;
        float _maxCurrent;
        ControlPilotStatus _status;
        Ticker _statusTicker;

        static void determineStatus(IEC61851ControlPilot* instancePtr); 
};