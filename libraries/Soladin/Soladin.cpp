#include <Soladin.h>
#include <Arduino.h>
#include <Tracer.h>
#include <PrintHex.h>

#define DEBUG_BAUDRATE 74880

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
  SoladinProbeResponse response;
  return query((byte*) _cmdProbe, (byte*) &response, 9);
}


bool SoladinComm::getDeviceStats()
{
  SoladinDeviceStatsResponse response;
  if (!query((byte*) _cmdDeviceStats, (byte*) &response, 31))
    return false;
  
  PvVoltage = float(response.pvVoltage) / 10;
  PvCurrent = float(response.pvCurrent) / 100;
  GridFrequency = float(response.gridFrequency) / 100;
  GridVoltage = response.gridVoltage;
  GridPower = response.gridPower;
  word gridEnergy = word(response.gridEnergy[1], response.gridEnergy[0]);
  gridEnergy += static_cast<word>(response.gridEnergy[2]) << 16;
  GridEnergy = float(gridEnergy) / 100;
  Temperature = response.temperature;

  Flags = "";
  int bitValue = 1;
  for (int i = 0; i < 12; i++)
  {
    if (response.flags & bitValue)
    {
      Flags += _flags[i];
      Flags += " ";
    }
    bitValue = bitValue << 1;
  }

  Serial.printf("Flags: 0x%04X -> ", response.flags);
  Serial.println(Flags);
  Serial.printf("PV Voltage: %f V\n", PvVoltage);
  Serial.printf("PV Current: %f A\n", PvCurrent);
  Serial.printf("Grid Frequency: %f Hz\n", GridFrequency);
  Serial.printf("Grid Voltage: %d V\n", GridVoltage);
  Serial.printf("Grid Power: %d W\n", GridPower);
  Serial.printf("Grid Energy: %f kWh\n", GridEnergy);
  Serial.printf("Temperature: %d degrees\n", Temperature);

  return true;
}


bool SoladinComm::query(byte* cmd, byte* response, int responseSize)
{
  Tracer tracer("SoladinComm::query");

  printHex(cmd, 9);
  Serial.printf("Response size: %d\n", responseSize);

  // Switch Serial to Soladin
  Serial.flush();
  delay(50);
  Serial.begin(9600); // Soladin uses 9600 8N1
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
  Serial.begin(DEBUG_BAUDRATE);
  resetGpio(false);

  printHex(response, bytesRead);
  Serial.printf("%d bytes read\n%d bytes garbage\n", bytesRead, garbageRead);

  return result;
}
