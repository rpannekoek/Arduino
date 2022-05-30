struct EnergyLogEntry
{
    time_t time;
    float onDuration = 0; // hours
    uint16_t maxPower = 0; // Watts
    float energy = 0.0; // Wh or kWh

    void update(uint16_t power, float duration, bool kWh)
    {
        if (power > 0) onDuration += duration;
        maxPower = std::max(maxPower, power);
        float energyDelta = duration * power;
        energy += kWh ? energyDelta / 1000 : energyDelta; 
    }
};
