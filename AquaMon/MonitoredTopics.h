#include "Aquarea.h"

#define NUMBER_OF_MONITORED_TOPICS 15

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
        snprintf(format, sizeof(format), "%%0.%df", decimals + additionalDecimals);

        // Then format the value
        static char buffer[16];
        snprintf(buffer, sizeof(buffer), format, value);

        if (includeUnitOfMeasure)
        {
            strcat(buffer, " ");
            strcat_P(buffer, unitOfMeasure);
        }
        return buffer;
    }
};


struct TopicLogEntry
{
    time_t time;
    float topicValues[NUMBER_OF_MONITORED_TOPICS];

    bool equals(TopicLogEntry* otherPtr)
    {
        for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        {
            if (topicValues[i] != otherPtr->topicValues[i])
                return false;
        }
        return true;
    }

    void reset()
    {
        memset(topicValues, 0, sizeof(topicValues));
    }
};


MonitoredTopic MonitoredTopics[] PROGMEM =
{
    { TopicId::Main_Inlet_Temp, "Tinlet", "T<sub>inlet</sub>", "°C", "temp1", 1, 0, 60 },
    { TopicId::Main_Outlet_Temp, "Toutlet", "T<sub>outlet</sub>", "°C", "temp1", 1, 0, 60 },
    { TopicId::Z1_Water_Temp, "Tzone1", "T<sub>zone1</sub>", "°C", "temp1", 0, 0, 60 },
    { TopicId::Buffer_Temp, "Tbuffer", "T<sub>buffer</sub>", "°C", "temp1", 0, 0, 60 },
    { TopicId::Solar_DeltaT, "dTsolar", "ΔT<sub>solar</sub>", "°C", "deltat", 0, 0, 15 },
    { TopicId::Solar_Temp, "Tsolar", "T<sub>solar</sub>", "°C", "temp2", 0, 0, 120 },
    { TopicId::Discharge_Temp, "Tdischarge", "T<sub>discharge</sub>", "°C", "temp2", 0, 0, 120 },
    { TopicId::Outside_Pipe_Temp, "Tpipe", "T<sub>pipe</sub>", "°C", "temp3", 0, -10, 20 },
    { TopicId::Outside_Temp, "Toutside", "T<sub>outside</sub>", "°C", "temp3", 0, -10, 20 },
    { TopicId::Defrosting_State, "Defrost", "Defrost", "", "in", 0, 0, 1 },
    { TopicId::Fan1_Motor_Speed, "Fan", "Fan", "rpm", "flow", 0, 0, 900 },
    { TopicId::Pump_Flow, "Qpump", "Q<sub>pump</sub>", "l/min", "flow", 1, 0, 30 },
    { TopicId::Compressor_Freq, "Fcomp", "F<sub>comp</sub>", "Hz", "freq", 0, 0, 60},
    { TopicId::Compressor_Power, "Pcomp", "P<sub>comp</sub>", "kW", "in", 1, 0, 6 },
    { TopicId::Heat_Power, "Pheat", "P<sub>heat</sub>", "kW", "out", 1, 0, 6 },
};
