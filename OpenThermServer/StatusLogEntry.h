struct __attribute__ ((packed)) StatusLogEntry
{
    time_t startTime;
    time_t stopTime;
    uint32_t chSeconds = 0;
    uint32_t dhwSeconds = 0;
    uint32_t flameSeconds = 0;
};
