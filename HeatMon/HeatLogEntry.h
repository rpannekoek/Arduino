struct HeatLogEntry
{
    time_t time;
    float minTInput = 100;
    float maxTInput = 0;
    float sumTInput = 0;
    float minTOutput = 100;
    float maxTOutput = 0;
    float sumTOutput = 0;
    float minFlowRate = 100;
    float maxFlowRate = 0;
    float sumFlowRate = 0;
    float minPOut = 10000;
    float maxPOut = 0;
    float sumPOut = 0;
    float minPIn = 10000;
    float maxPIn = 0;
    float sumPIn = 0;

    void update(float tInput, float tOutput, float flowRate, float pOut, float pIn)
    {
        minTInput = std::min(minTInput, tInput);
        maxTInput = std::max(maxTInput, tInput);
        sumTInput += tInput;

        minTOutput = std::min(minTOutput, tOutput);
        maxTOutput = std::max(maxTOutput, tOutput);
        sumTOutput += tOutput;

        minFlowRate = std::min(minFlowRate, flowRate);
        maxFlowRate = std::max(maxFlowRate, flowRate);
        sumFlowRate += flowRate;

        minPOut = std::min(minPOut, pOut);
        maxPOut = std::max(maxPOut, pOut);
        sumPOut += pOut;

        minPIn = std::min(minPIn, pIn);
        maxPIn = std::max(maxPIn, pIn);
        sumPIn += pIn;
    }
};
