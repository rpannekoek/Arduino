struct ChargeStatsEntry
{
    time_t startTime;
    time_t endTime;
    float energy; // Wh
    float temperatureSum;
    int32_t count;

    float getDurationHours()
    {
        return float(endTime - startTime) / SECONDS_PER_HOUR;
    }

    float getAvgPower()
    {
        float duration = getDurationHours(); 
        return (duration == 0) ? 0 : (energy / duration);
    }

    float getAvgTemperature()
    {
        return (count == 0) ? 0 : (temperatureSum / count);
    }

    void init(time_t time)
    {
        startTime = time;
        endTime = time;
        energy = 0;
        temperatureSum = 0;
        count = 0;
    }

    void update(time_t time, float power, float temperature)
    {
        float interval = float(time - endTime) / SECONDS_PER_HOUR;

        endTime = time;
        energy += power * interval;
        temperatureSum += temperature;
        count++;
    }
};