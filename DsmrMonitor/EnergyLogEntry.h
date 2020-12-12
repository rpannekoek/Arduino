#include <Tracer.h>

struct EnergyLogEntry
{
    time_t time;
    uint16_t maxPowerDelivered = 0; // Watts
    uint16_t maxPowerReturned = 0; // Watts
    uint16_t maxPowerGas = 0; // Watts
    float energyDelivered = 0.0; // Wh or kWh
    float energyReturned = 0.0; // Wh or kWh
    float energyGas = 0.0; // Wh or kWh

    void update(
        float powerDelivered,
        float powerReturned,
        float powerGas,
        float hoursSinceLastUpdate,
        float scale
        )
    {
        TRACE(F("EnergyLogEntry::update(%0.0f, %0.0f, %0.0f, %f, %0.0f)\n"), 
            powerDelivered, powerReturned, powerGas, hoursSinceLastUpdate, scale);

        energyDelivered += powerDelivered * hoursSinceLastUpdate / scale;
        energyReturned += powerReturned * hoursSinceLastUpdate / scale;
        energyGas += powerGas * hoursSinceLastUpdate / scale;

        if (powerDelivered > maxPowerDelivered) maxPowerDelivered = powerDelivered;
        if (powerReturned > maxPowerReturned) maxPowerReturned = powerReturned;
        if (powerGas > maxPowerGas) maxPowerGas = powerGas;
    }
};
