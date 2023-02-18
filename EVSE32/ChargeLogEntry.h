struct ChargeLogEntry
{
    time_t time;
    float currentLimit;
    float outputCurrent;
    float temperature;

    bool equals(ChargeLogEntry* otherPtr)
    {
        return std::abs(currentLimit - otherPtr->currentLimit) < 0.1
            && std::abs(outputCurrent - otherPtr->outputCurrent) < 0.1
            && std::abs(temperature - otherPtr->temperature) < 0.1;
    }
};