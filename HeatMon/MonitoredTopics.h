#define NUMBER_OF_TOPICS 7

enum TopicId
{
    TInput = 0,
    TOutput,
    TBuffer,
    DeltaT,
    FlowRate,
    POut,
    PIn
};


struct TopicStats
{
    float min = 666;
    float max = 0;
    float sum = 0;

    void update(float topicValue)
    {
        min = std::min(min, topicValue);
        max = std::max(max, topicValue);
        sum += topicValue;
    }
};


struct HeatLogEntry
{
    time_t time;
    uint32_t count = 0;
    uint32_t valveActivatedSeconds = 0;

    TopicStats topicStats[NUMBER_OF_TOPICS];

    float inline getAverage(TopicId topicId)
    {
        return (count == 0) ? 0 : (topicStats[topicId].sum / count);
    }

    void update(float* topicValues, uint32_t valveSeconds)
    {
        count++;
        valveActivatedSeconds += valveSeconds;

        for (int i = 0; i < NUMBER_OF_TOPICS; i++)
        {
            topicStats[i].update(topicValues[i]);
        }
    }
};


struct MonitoredTopic
{
    TopicId id;
    const char* label;
    const char* htmlLabel;
    const char* unitOfMeasure;
    const char* style;
    int decimals;
    int minValue;
    int maxValue;

    const char* formatValue(float value, bool includeUnitOfMeasure, int additionalDecimals = 0)
    {
        // First build the format string
        static char buffer[16];
        if (includeUnitOfMeasure)
            snprintf(buffer, sizeof(buffer), "%%0.%df %s", decimals + additionalDecimals, unitOfMeasure);
        else
            snprintf(buffer, sizeof(buffer), "%%0.%df", decimals + additionalDecimals);

        // Then format the value
        snprintf(buffer, sizeof(buffer), buffer, value);

        return buffer;
    }
};


MonitoredTopic MonitoredTopics[] =
{
    { TopicId::TInput, "Tin", "T<sub>in</sub>", "°C", "water", 1, 20, 60 },
    { TopicId::TOutput, "Tout", "T<sub>out</sub>", "°C", "water", 1, 20, 60 },
    { TopicId::TBuffer, "Tbuffer", "T<sub>buffer</sub>", "°C", "water", 1, 20, 90 },
    { TopicId::DeltaT, "DeltaT", "ΔT", "°C", "deltat", 1, 0, 20 },
    { TopicId::FlowRate, "Flow", "Flow rate", "l/min", "flow", 1, 0, 15 },
    { TopicId::POut, "Pout", "P<sub>out</sub>", "kW", "power", 1, 0, 10 },
    { TopicId::PIn, "Pin", "P<sub>in</sub>", "kW", "pIn", 2, 0, 4 },
};