struct OpenThermLogEntry
{
    time_t time;
    uint16_t thermostatTSet;
    uint16_t thermostatMaxRelModulation;
    uint16_t boilerStatus;
    uint16_t boilerTSet;
    uint16_t boilerTWater;
    uint16_t tOutside;
    uint8_t repeat;

    bool equals(OpenThermLogEntry* otherPtr)
    {
        return (otherPtr->thermostatTSet == thermostatTSet) &&
            (otherPtr->thermostatMaxRelModulation == thermostatMaxRelModulation) &&
            (otherPtr->boilerStatus == boilerStatus) &&
            (otherPtr->boilerTSet == boilerTSet) &&
            (otherPtr->boilerTWater == boilerTWater) &&
            (otherPtr->tOutside == tOutside);
    }
};
