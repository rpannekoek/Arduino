#define NUMBER_OF_MONITORED_TOPICS 5

enum TopicId
{
    Temperature,
    Pressure,
    Humidity,
    CO2,
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

    String formatValue(float value, bool includeUnitOfMeasure, int additionalDecimals = 0)
    {
        // First build the format string
        char format[16];
        snprintf(format, sizeof(format), "%%0.%df%%s%%s", decimals + additionalDecimals);

        // Then format the value & unit of measure
        char buffer[16];
        if (includeUnitOfMeasure)
            snprintf(buffer, sizeof(buffer), format, value, " ", unitOfMeasure);
        else
            snprintf(buffer, sizeof(buffer), format, value, "", "");

        return String(buffer);
    }
};


struct TopicLogEntry
{
    time_t time;
    uint32_t count = 0;
    float topicValues[NUMBER_OF_MONITORED_TOPICS];

    float inline getAverage(int topicId)
    {
        return (count == 0) ? 0.0F : topicValues[topicId] / count;
    }

    bool equals(TopicLogEntry* otherPtr)
    {
        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            float otherAvg = otherPtr->getAverage(i);
            if (otherAvg == 0.0F)
            {
                if (getAverage(i) != 0.0F) return false;
            }
            else if (std::abs(getAverage(i) - otherAvg) / otherAvg >= 0.01) // +/- 1%
                return false;
        }
        return true;
    }

    void aggregate(float* values)
    {
        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            topicValues[i] += values[i];
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
    { TopicId::Pressure, "Pressure", "Pressure", "hPa", "pressure", 0, 900, 1100 },
    { TopicId::Humidity, "Humidity", "Humidity", "%", "humidity", 0, 0, 100 },
    { TopicId::CO2, "CO2", "CO<sub>2</sub>", "ppm", "iaq", 0, 400, 1600 },
    { TopicId::Fan, "Fan", "Fan", "", "fan", 0, 0, 1 }
};