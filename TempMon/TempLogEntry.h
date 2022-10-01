struct TempLogEntry
{
    time_t time;
    uint32_t count = 0;
    float sumTinside = 0;
    float sumToutside = 0;

    float getAvgTinside()
    {
        return (count == 0) ? 0 : sumTinside / count;
    }

    float getAvgToutside()
    {
        return (count == 0) ? 0 : sumToutside / count;
    }

    bool equals(TempLogEntry* otherPtr)
    {
        return (std::abs(getAvgTinside() - otherPtr->getAvgTinside()) < 0.1)
            && (std::abs(getAvgToutside() - otherPtr->getAvgToutside()) < 0.1);
    }

    void update(float tInside, float tOutside)
    {
        sumTinside += tInside;
        sumToutside += tOutside;
        count++;
    }

    void reset()
    {
        sumTinside = 0;
        sumToutside = 0;
        count = 0;
    }
};
