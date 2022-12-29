#define NUMBER_OF_MONITORED_TOPICS 8

enum TopicId
{
    Temperature,
    Pressure,
    Humidity,
    IAQ,
    CO2Equivalent,
    BVOCEquivalent,
    Accuracy,
    Fan
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
        static char format[16];
        snprintf(format, sizeof(format), "%%0.%df%%s%%s", decimals + additionalDecimals);

        // Then format the value & unit of measure
        static char buffer[16];
        if (includeUnitOfMeasure)
            snprintf(buffer, sizeof(buffer), format, value, " ", unitOfMeasure);
        else
            snprintf(buffer, sizeof(buffer), format, value, "", "");

        return buffer;
    }
};


struct TopicLogEntry
{
    time_t time;
    uint32_t count = 0;
    float topicValues[NUMBER_OF_MONITORED_TOPICS];

    float getAverage(int topicId)
    {
        return (count == 0) ? 0.0F : topicValues[topicId] / count;
    }

    bool equals(TopicLogEntry* otherPtr)
    {
        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            if (std::abs(topicValues[i]- otherPtr->topicValues[i]) >= 0.1)
                return false;
        }
        return true;
    }

    void aggregate(TopicLogEntry* otherPtr)
    {
        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            topicValues[i] += otherPtr->topicValues[i];
        }
        count++;
    }

    void reset()
    {
        memset(topicValues, 0, sizeof(topicValues));
        count = 0;
    }
};


MonitoredTopic MonitoredTopics[] =
{
    { TopicId::Temperature, "Temperature", "Temperature", "°C", "temperature", 1, 0, 30 },
    { TopicId::Pressure, "Pressure", "Pressure", "hPa", "pressure", 0, 900, 1200 },
    { TopicId::Humidity, "Humidity", "Humidity", "%", "humidity", 0, 0, 100 },
    { TopicId::IAQ, "IAQ", "IAQ", "", "iaq", 0, 0, 300 },
    { TopicId::CO2Equivalent, "CO2", "CO<sub>2</sub>", "ppm", "ppm", 0, 400, 1600 },
    { TopicId::BVOCEquivalent, "BVOC", "BVOC", "ppm", "ppm", 0, 0, 1500 },
    { TopicId::Accuracy, "Accuracy", "Accuracy", "", "accuracy", 0, 0, 3 },
    { TopicId::Fan, "Fan", "Fan", "", "fan", 0, 0, 1 }
};