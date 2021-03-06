struct PhaseData
{
    float voltage;
    float current;
    float powerDelivered = 0;
    float powerReturned = 0;

    void update(float newVoltage, float newCurrent, float newPowerDelivered, float newPowerReturned)
    {
        voltage = newVoltage;
        current = newCurrent;
        powerDelivered = newPowerDelivered * 1000; // Watts
        powerReturned = newPowerReturned * 1000; // Watts
    }
};
