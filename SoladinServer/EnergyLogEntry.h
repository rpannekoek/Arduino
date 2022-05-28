struct EnergyLogEntry
{
    time_t time;
    float onDuration = 0; // hours
    uint16_t maxPower = 0; // Watts
    float energy = 0.0; // Wh or kWh
};
