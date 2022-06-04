struct PowerLogEntry
{
    time_t time;
    uint16_t powerDelivered[3];
    uint16_t powerReturned[3];
    uint16_t powerGas;
};