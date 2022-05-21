struct TempLogEntry
{
    time_t time;

    float minTInside = 100;
    float maxTInside = -100;
    float sumTInside = 0;

    float minTOutside = 100;
    float maxTOutside = -100;
    float sumTOutside = 0;
    void update(float tInside, float tOutside)
    {
        minTInside = std::min(minTInside, tInside);
        maxTInside = std::max(maxTInside, tInside);
        sumTInside += tInside;

        minTOutside = std::min(minTOutside, tOutside);
        maxTOutside = std::max(maxTOutside, tOutside);
        sumTOutside += tOutside;
    }
};
