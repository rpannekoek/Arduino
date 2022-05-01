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
    uint16_t pHeatPump; // kW in OT f8.8 format

    bool equals(OpenThermLogEntry* otherPtr)
    {
        return (otherPtr->thermostatTSet == thermostatTSet) &&
            (otherPtr->thermostatMaxRelModulation == thermostatMaxRelModulation) &&
            (otherPtr->boilerStatus == boilerStatus) &&
            (otherPtr->boilerTSet == boilerTSet) &&
            (otherPtr->tBoiler == tBoiler) &&
            (otherPtr->tReturn == tReturn) &&
            (otherPtr->tBuffer == tBuffer) &&
            (otherPtr->tOutside == tOutside) &&
            (otherPtr->pHeatPump == pHeatPump);
    }
};
