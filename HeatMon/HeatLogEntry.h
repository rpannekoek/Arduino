struct HeatLogEntry
{
    time_t time;
    time_t valveActivatedSeconds = 0;

    float minTInput = 100;
    float maxTInput = 0;
    float sumTInput = 0;

    float minTOutput = 100;
    float maxTOutput = 0;
    float sumTOutput = 0;

    float minTBuffer = 100;
    float maxTBuffer = 0;
    float sumTBuffer = 0;

    float minDeltaT = 100;
    float maxDeltaT = 0;
    float sumDeltaT = 0;

    float minFlowRate = 100;
    float maxFlowRate = 0;
    float sumFlowRate = 0;

    float minPOut = 10000;
    float maxPOut = 0;
    float sumPOut = 0;

    float minPIn = 10000;
    float maxPIn = 0;
    float sumPIn = 0;

    void update(float tInput, float tOutput, float tBuffer, float flowRate, float pOut, float pIn, bool valveActivated)
    {
        float deltaT = tInput - tOutput;

        if (valveActivated)
            valveActivatedSeconds++;
 
        minTInput = std::min(minTInput, tInput);
        maxTInput = std::max(maxTInput, tInput);
        sumTInput += tInput;

        minTOutput = std::min(minTOutput, tOutput);
        maxTOutput = std::max(maxTOutput, tOutput);
        sumTOutput += tOutput;

        minTBuffer = std::min(minTBuffer, tBuffer);
        maxTBuffer = std::max(maxTBuffer, tBuffer);
        sumTBuffer += tBuffer;

        minDeltaT = std::min(minDeltaT, deltaT);
        maxDeltaT = std::max(maxDeltaT, deltaT);
        sumDeltaT += deltaT;

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
