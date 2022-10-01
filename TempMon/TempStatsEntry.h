struct TempStatsEntry
{
    time_t time;
    uint32_t count = 0;

    float minTInside = 100;
    float maxTInside = -100;
    float sumTInside = 0;

    float minTOutside = 100;
    float maxTOutside = -100;
    float sumTOutside = 0;

    float getAvgTInside()
    {
        return (count == 0) ? 0 : sumTInside / count;
    }

    float getAvgTOutside()
    {
        return (count == 0) ? 0 : sumTOutside / count;
    }

    void update(float tInside, float tOutside)
    {
        minTInside = std::min(minTInside, tInside);
        maxTInside = std::max(maxTInside, tInside);
        sumTInside += tInside;

        minTOutside = std::min(minTOutside, tOutside);
        maxTOutside = std::max(maxTOutside, tOutside);
        sumTOutside += tOutside;

        count++;
    }
};
