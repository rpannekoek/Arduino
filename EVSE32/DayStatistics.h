struct DayStatistics
{
    float tMin = 666;
    float tMax = -666;

    time_t tMinTime = 0;
    time_t tMaxTime = 0;

    void update(time_t time, float temperature)
    {
        if (temperature < tMin || (time - tMinTime) > (24 * 3600))
        {
            tMin = temperature;
            tMinTime = time;
        }
        if (temperature > tMax || (time - tMaxTime) > (24 * 3600))
        {
            tMax = temperature;
            tMaxTime = time;
        }
    }

    void reset()
    {
        tMin = 666;
        tMax = -666;
        tMinTime = 0;
        tMaxTime = 0;
    }
};