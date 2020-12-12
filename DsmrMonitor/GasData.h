#include <WString.h>
#include <Tracer.h>

struct GasData
{
    String timestamp;
    time_t time = 0;
    float energy = 0; // kWh
    float power = 0;

    void update(String& newTimestamp, time_t newTime, float newEnergy)
    {
        timestamp = newTimestamp;
        if (time > 0)
        {
            float deltaEnergy = (newEnergy - energy) * 1000; // Wh
            float deltaTime = float(newTime - time) / 3600; // hours
            power = deltaEnergy / deltaTime;
            TRACE(F("Delta energy: %0.0f Wh in %f h. Power: %0.0f W\n"), deltaEnergy, deltaTime, power);
        }
        time = newTime;
        energy = newEnergy;
    }
};
