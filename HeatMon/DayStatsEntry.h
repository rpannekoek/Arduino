struct DayStatsEntry
{
    time_t time;
    uint32_t valveActivatedSeconds = 0;
    float energyOut = 0; // kWh
    float energyIn = 0; // kWh

    float inline getCOP()
    {
        return (energyIn == 0) ? 0 : (energyOut / energyIn);
    }
};
