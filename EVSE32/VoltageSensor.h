class VoltageSensor
{
    public:
        VoltageSensor(uint8_t pin);

        bool begin();

        bool detectSignal(uint32_t sensePeriodMs = 100);

    private:
        uint8_t _pin;
};