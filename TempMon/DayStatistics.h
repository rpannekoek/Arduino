struct DayStatistics
{
    float tInsideMin = 100;
    float tInsideMax = -100;
    float tOutsideMin = 100;
    float tOutsideMax = -100;

    time_t insideMinTime = 0;
    time_t insideMaxTime = 0;
    time_t outsideMinTime = 0;
    time_t outsideMaxTime = 0;

    void update(float tInside, float tOutside, time_t time)
    {
        if (tInside < tInsideMin)
        {
            tInsideMin = tInside;
            insideMinTime = time;
        }
        if (tInside > tInsideMax)
        {
            tInsideMax = tInside;
            insideMaxTime = time;
        }
        if (tOutside < tOutsideMin)
        {
            tOutsideMin = tOutside;
            outsideMinTime = time;
        }
        if (tInside > tOutsideMax)
        {
            tOutsideMax = tOutside;
            outsideMaxTime = time;
        }
    }

    void reset()
    {
        tInsideMin = 100;
        tInsideMax = -100;
        tOutsideMin = 100;
        tOutsideMax = -100;

        insideMinTime = 0;
        insideMaxTime = 0;
        outsideMinTime = 0;
        outsideMaxTime = 0;
    }
};