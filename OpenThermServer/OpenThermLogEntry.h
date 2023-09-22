struct __attribute__ ((packed)) OpenThermLogEntry
{
    time_t time;
    uint16_t thermostatTSet;
    uint16_t thermostatMaxRelModulation;
    uint16_t boilerStatus;
    uint16_t boilerTSet;
    uint16_t tBoiler;
    uint16_t tReturn;
    uint16_t tBuffer;
    uint16_t tOutside;
    uint16_t pressure;
    uint16_t pHeatPump; // kW in OT f8.8 format

    bool equals(OpenThermLogEntry* otherPtr)
    {
        return (otherPtr->thermostatTSet == thermostatTSet) &&
            (otherPtr->thermostatMaxRelModulation == thermostatMaxRelModulation) &&
            (otherPtr->boilerStatus == boilerStatus) &&
            (otherPtr->boilerTSet == boilerTSet) &&
            isSimilar(otherPtr->tBoiler, tBoiler) &&
            isSimilar(otherPtr->tReturn, tReturn) &&
            isSimilar(otherPtr->tBuffer, tBuffer) &&
            isSimilar(otherPtr->tOutside, tOutside) &&
            isSimilar(otherPtr->pressure, pressure, 4) &&
            isSimilar(otherPtr->pHeatPump, pHeatPump, 4);
    }

    static bool isSimilar(int lhs, int rhs, int maxDiff = 32)
    {
        return abs(rhs - lhs) < maxDiff;
    }
};
