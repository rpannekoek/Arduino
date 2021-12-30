struct GlobalLogEntry
{
    time_t time;
    uint32_t flameCount = 0;
    float minTWater = 100;
    float maxTWater = 0;
    float sumTWater = 0;
    float minTReturn = 100;
    float maxTReturn = 0;
    float sumTReturn = 0;

    void update(float tWater, float tReturn, bool flame)
    {
        minTWater = std::min(minTWater, tWater);
        maxTWater = std::max(maxTWater, tWater);
        sumTWater += tWater;

        minTReturn = std::min(minTReturn, tReturn);
        maxTReturn = std::max(maxTReturn, tReturn);
        sumTReturn += tReturn;

        if (flame) flameCount++;
    }
};
