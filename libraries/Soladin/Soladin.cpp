#include <Soladin.h>
#include <Arduino.h>
#include <Tracer.h>
#include <PrintHex.h>
#include <PrintFlags.h>


const char* FLAG_NAMES[12] = {"Vpv+", "Vpv-", "!Vac", "Vac+", "Vac-", "Fac+", "Fac-", "T+", "HW-ERR", "Start", "Pmax", "Imax"};


struct SoladinProbeResponse
{
  uint16_t destinationId;
  uint16_t sourceId;
  uint16_t commandId;
  uint16_t unknown;
  byte checkSum; 
};


struct SoladinDeviceStatsResponse
{
  uint16_t destinationId;
  uint16_t sourceId;
  uint16_t commandId;
  uint16_t flags;
  uint16_t pvVoltage;
  uint16_t pvCurrent;
  uint16_t gridFrequency;
  uint16_t gridVoltage;
  uint16_t unknown1;
  uint16_t gridPower;
  byte gridEnergy[3];
  byte temperature;
  uint32_t operatingTime;
  uint16_t unknown2;
  byte checkSum; 
};


// Constructor
SoladinComm::SoladinComm()
{
    // The Serial port (UART0) is used to communicate with the computer (USB/Serial) and with Soladin (RS422).
    // The USB/Serial adapter uses standard UART pins GPIO1 (TX) and GPIO3 (RX).
    // For communication with Soladin, the UART is swapped to use GPIO15 (TX) and GPIO13 (RX).
    // We initialize GPIO1 and GPIO15 to be an output in high state so TX is in determinate state when the UART is swapped.
    pinMode(1, OUTPUT);
    pinMode(3, INPUT);
    pinMode(13, INPUT);
    pinMode(15, OUTPUT);
    digitalWrite(1, 1);
    digitalWrite(15, 1);
}


void SoladinComm::resetGpio(bool swap)
{
    if (swap)
    {
        pinMode(1, OUTPUT);
        digitalWrite(1, 1);
    }
    else
    {
        pinMode(15, OUTPUT);
        digitalWrite(15, 1);
    }
}


bool SoladinComm::probe()
{
    Tracer tracer(F("SoladinComm::probe"));

    SoladinProbeResponse response;
    return query((byte*) _cmdProbe, (byte*) &response, 9);
}


bool SoladinComm::getDeviceStats()
{
    Tracer tracer(F("SoladinComm::getDeviceStats"));

    SoladinDeviceStatsResponse response;
    if (!query((byte*) _cmdDeviceStats, (byte*) &response, 31))
        return false;
    
    pvVoltage = float(response.pvVoltage) / 10;
    pvCurrent = float(response.pvCurrent) / 100;
    gridFrequency = float(response.gridFrequency) / 100;
    gridVoltage = response.gridVoltage;
    gridPower = response.gridPower;
    uint32_t nrg = word(response.gridEnergy[1], response.gridEnergy[0]);
    nrg += response.gridEnergy[2] << 16;
    gridEnergy = float(nrg) / 100;
    temperature = response.temperature;

    flags = printFlags(response.flags, FLAG_NAMES, 12, " ");

    TRACE(F("Flags: 0x%04X -> %s\n"), response.flags, flags.c_str());
    TRACE(F("PV Voltage: %f V\n"), pvVoltage);
    TRACE(F("PV Current: %f A\n"), pvCurrent);
    TRACE(F("Grid Frequency: %f Hz\n"), gridFrequency);
    TRACE(F("Grid Voltage: %d V\n"), gridVoltage);
    TRACE(F("Grid Power: %d W\n"), gridPower);
    TRACE(F("Grid Energy: %f kWh\n"), gridEnergy);
    TRACE(F("Temperature: %d degrees\n"), temperature);

    return true;
}


bool SoladinComm::query(byte* cmd, byte* response, int responseSize)
{
    Tracer tracer(F("SoladinComm::query"));

    printHex(cmd, 9);
    TRACE(F("Response size: %d\n"), responseSize);

    int originalBaudRate = Serial.baudRate();

    // Switch Serial to Soladin
    Serial.begin(9600); // Soladin uses 9600 8N1
    Serial.flush();
    delay(50);
    Serial.swap(); // Use GPIO13 (RX) and GPIO15 (TX)
    resetGpio(true);

    // Get rid of garbage in input
    Serial.setTimeout(10);
    int garbageRead = Serial.readBytes(response, responseSize);  

    // Write command to Soladin
    Serial.write(cmd, 9);

    // Await response from Soladin
    Serial.setTimeout(1000);
    int bytesRead = Serial.readBytes(response, responseSize);  
    bool result = (bytesRead == responseSize);

    // Switch Serial back for debug output
    Serial.flush();
    Serial.begin(originalBaudRate);
    resetGpio(false);

    printHex(response, bytesRead);
    TRACE(F("%d bytes read\n%d bytes garbage\n"), bytesRead, garbageRead);

    return result;
}
