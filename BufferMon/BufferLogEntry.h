struct BufferLogEntry
{
    time_t time;
    time_t valveActivatedSeconds = 0;
    float minTemp = 100;
    float maxTemp = 0;
    float sumTemp = 0;

    void update(float temperature, bool valveActivated)
    {
        minTemp = std::min(minTemp, temperature);
        maxTemp = std::max(maxTemp, temperature);
        sumTemp += temperature;

        if (valveActivated)
        {
            valveActivatedSeconds++;
        }
    }
};
