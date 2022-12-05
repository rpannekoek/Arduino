struct DayStatsEntry
{
    time_t startTime;
    time_t stopTime;
    uint32_t antiFreezeSeconds = 0;
    uint32_t onSeconds = 0;
    uint32_t onCount = 0;
    uint32_t defrosts = 0;
    float energyIn = 0; // kWh
    float energyOut = 0; // kWh


    uint32_t inline getAvgOnSeconds()
    {
        return (onCount == 0) ? 0 : onSeconds / onCount;
    }


    float inline getCOP()
    {
        return (energyIn == 0) ? 0 : energyOut / energyIn;
    }


    void update(time_t time, uint32_t secondsSinceLastUpdate, float powerInKW, float powerOutKW, bool antiFreezeActivated)
    {
        float hoursSinceLastUpdate = float(secondsSinceLastUpdate) / 3600;

        if (powerInKW > 0)
        {
            if (onSeconds == 0)
                startTime = time;
            stopTime = time;
            onSeconds += secondsSinceLastUpdate;
            energyIn += powerInKW * hoursSinceLastUpdate;
        }

        if (antiFreezeActivated)
            antiFreezeSeconds += secondsSinceLastUpdate;

        energyOut += powerOutKW * hoursSinceLastUpdate;
    }
};
