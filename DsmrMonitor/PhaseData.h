struct PhaseData
{
    String label;
    float voltage;
    float current;
    float powerDelivered = 0;
    float powerReturned = 0;
    uint32_t sumPowerDelivered = 0;
    uint32_t sumPowerReturned = 0;
    int count = 0;

    uint16_t getAvgPowerDelivered()
    {
        return (count == 0) ? 0 : sumPowerDelivered / count;
    }

    uint16_t getAvgPowerReturned()
    {
        return (count == 0) ? 0 : sumPowerReturned / count;
    }


    void update(float newVoltage, float newCurrent, float newPowerDelivered, float newPowerReturned)
    {
        voltage = newVoltage;
        current = newCurrent;
        powerDelivered = newPowerDelivered * 1000; // Watts
        powerReturned = newPowerReturned * 1000; // Watts
        sumPowerDelivered += powerDelivered;
        sumPowerReturned += powerReturned;
        count++;
    }

    void reset()
    {
        sumPowerDelivered = 0;
        sumPowerReturned = 0;
        count = 0;
    }
};
