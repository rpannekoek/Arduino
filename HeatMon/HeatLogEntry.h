struct HeatLogEntry
{
    time_t time;
    float minTInput = 100;
    float maxTInput = 0;
    float sumTInput = 0;
    float minTOutput = 100;
    float maxTOutput = 0;
    float sumTOutput = 0;
    float minFlow = 100;
    float maxFlow = 0;
    float sumFlow = 0;
    float minPower = 10000;
    float maxPower = 0;
    float sumPower = 0;

    void update(float tInput, float tOutput, float flow, float power)
    {
        minTInput = std::min(minTInput, tInput);
        maxTInput = std::max(maxTInput, tInput);
        sumTInput += tInput;

        minTOutput = std::min(minTOutput, tOutput);
        maxTOutput = std::max(maxTOutput, tOutput);
        sumTOutput += tOutput;

        minFlow = std::min(minFlow, flow);
        maxFlow = std::max(maxFlow, flow);
        sumFlow += flow;

        minPower = std::min(minPower, power);
        maxPower = std::max(maxPower, power);
        sumPower += power;
    }
};
