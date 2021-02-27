struct GlobalLogEntry
{
    time_t time;
    uint32_t flameCount = 0;
    float minTWater = 100;
    float maxTWater = 0;
    float sumTWater = 0;

    void update(float tWater, bool flame)
    {
        minTWater = std::min(minTWater, tWater);
        maxTWater = std::max(maxTWater, tWater);
        sumTWater += tWater;
        if (flame) flameCount++;
    }
};
