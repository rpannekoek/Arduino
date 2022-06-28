#include <Arduino.h>
#include <Tracer.h>
#include "Aquarea.h"

#define NUMBER_OF_TOPICS 109
#define AQUAREA_COMMAND_DATA_SIZE 108
#define AQUAREA_RESPONSE_DATA_SIZE 200

#define AQUAREA_QUERY_MAGIC 0x71
#define AQUAREA_RESPONSE_MAGIC 0x71

static uint8_t _queryData[AQUAREA_COMMAND_DATA_SIZE];
static uint8_t _commandData[AQUAREA_COMMAND_DATA_SIZE];

static const char* DisabledEnabled[] PROGMEM = {"2", "Disabled", "Enabled"};
static const char* BlockedFree[] PROGMEM = {"2", "Blocked", "Free"};
static const char* OffOn[] PROGMEM = {"2", "Off", "On"};
static const char* InactiveActive[] PROGMEM = {"2", "Inactive", "Active"};
static const char* HolidayState[] PROGMEM = {"3", "Off", "Scheduled", "Active"};
static const char* OpModeDesc[] PROGMEM = {"9", "Heat", "Cool", "Auto(heat)", "DHW", "Heat+DHW", "Cool+DHW", "Auto(heat)+DHW", "Auto(cool)", "Auto(cool)+DHW"};
static const char* Powerfulmode[] PROGMEM = {"4", "Off", "30min", "60min", "90min"};
static const char* Quietmode[] PROGMEM = {"4", "Off", "Level 1", "Level 2", "Level 3"};
static const char* Valve[] PROGMEM = {"2", "Room", "DHW"};
static const char* LitersPerMin[] PROGMEM = {"value", "l/min"};
static const char* RotationsPerMin[] PROGMEM = {"value", "r/min"};
static const char* Pressure[] PROGMEM = {"value", "kgf/cm2"};
static const char* Celsius[] PROGMEM = {"value", "°C"};
static const char* Kelvin[] PROGMEM = {"value", "K"};
static const char* Hertz[] PROGMEM = {"value", "Hz"};
static const char* Counter[] PROGMEM = {"value", "Count"};
static const char* Hours[] PROGMEM = {"value", "Hours"};
static const char* Watt[] PROGMEM = {"value", "Watt"};
static const char* KW[] PROGMEM = {"value", "kW"};
static const char* ErrorState[] PROGMEM = {"value", "Error"};
static const char* Ampere[] PROGMEM = {"value", "Ampere"};
static const char* Minutes[] PROGMEM = {"value", "Minutes"};
static const char* Duty[] PROGMEM = {"value", "Duty"};
static const char* ZonesState[] PROGMEM = {"3", "Zone1 active", "Zone2 active", "Zone1 and zone2 active"};
static const char* HeatCoolModeDesc[] PROGMEM = {"2", "Comp. Curve", "Direct"};
static const char* SolarModeDesc[] PROGMEM = {"3", "Disabled", "Buffer", "DHW"};
static const char* Model[] PROGMEM = {
    "21", //string representation of number of known models
    "WH-MDC05H3E5",
    "WH-MDC07H3E5",
    "IDU:WH-SXC09H3E5, ODU:WH-UX09HE5",
    "IDU:WH-SDC09H3E8, ODU:WH-UD09HE8",
    "IDU:WH-SXC09H3E8, ODU:WH-UX09HE8",
    "IDU:WH-SXC12H9E8, ODU:WH-UX12HE8",
    "IDU:WH-SXC16H9E8, ODU:WH-UX16HE8",
    "IDU:WH-SDC05H3E5, ODU:WH-UD05HE5",
    "IDU:WH-SDC0709J3E5, ODU:WH-UD09JE5",
    "WH-MDC05J3E5",
    "WH-MDC09H3E5",
    "WH-MXC09H3E5",
    "IDU:WH-ADC0309J3E5, ODU:WH-UD09JE5",
    "IDU:WH-ADC0916H9E8, ODU:WH-UX12HE8",
    "IDU:WH-SQC09H3E8, ODU:WH-UQ09HE8",
    "IDU:WH-SDC09H3E5, ODU:WH-UD09HE5",
    "IDU:WH-ADC0309H3E5, ODU:WH-UD09HE5",
    "IDU:WH-ADC0309J3E5, ODU: WH-UD05JE5",
    "IDU: WH-SDC0709J3E5, ODU: WH-UD07JE5",
    "IDU: WH-SDC07H3E5-1 ODU: WH-UD07HE5-1",
    "WH-MDC07J3E5",
};

static const uint8_t _knownModels[21][10] =
{
    0xE2, 0xCF, 0x0B, 0x13, 0x33, 0x32, 0xD1, 0x0C, 0x16, 0x33,
    0xE2, 0xCF, 0x0B, 0x14, 0x33, 0x42, 0xD1, 0x0B, 0x17, 0x33,
    0xE2, 0xCF, 0x0D, 0x77, 0x09, 0x12, 0xD0, 0x0B, 0x05, 0x11,
    0xE2, 0xCF, 0x0C, 0x88, 0x05, 0x12, 0xD0, 0x0B, 0x97, 0x05,
    0xE2, 0xCF, 0x0D, 0x85, 0x05, 0x12, 0xD0, 0x0C, 0x94, 0x05,
    0xE2, 0xCF, 0x0D, 0x86, 0x05, 0x12, 0xD0, 0x0C, 0x95, 0x05,
    0xE2, 0xCF, 0x0D, 0x87, 0x05, 0x12, 0xD0, 0x0C, 0x96, 0x05,
    0xE2, 0xCE, 0x0D, 0x71, 0x81, 0x72, 0xCE, 0x0C, 0x92, 0x81,
    0x62, 0xD2, 0x0B, 0x43, 0x54, 0x42, 0xD2, 0x0B, 0x72, 0x66,
    0xC2, 0xD3, 0x0B, 0x33, 0x65, 0xB2, 0xD3, 0x0B, 0x94, 0x65,
    0xE2, 0xCF, 0x0B, 0x15, 0x33, 0x42, 0xD1, 0x0B, 0x18, 0x33,
    0xE2, 0xCF, 0x0B, 0x41, 0x34, 0x82, 0xD1, 0x0B, 0x31, 0x35,
    0x62, 0xD2, 0x0B, 0x45, 0x54, 0x42, 0xD2, 0x0B, 0x47, 0x55,
    0xE2, 0xCF, 0x0C, 0x74, 0x09, 0x12, 0xD0, 0x0D, 0x95, 0x05,
    0xE2, 0xCF, 0x0B, 0x82, 0x05, 0x12, 0xD0, 0x0C, 0x91, 0x05,
    0xE2, 0xCF, 0x0C, 0x55, 0x14, 0x12, 0xD0, 0x0B, 0x15, 0x08,
    0xE2, 0xCF, 0x0C, 0x43, 0x00, 0x12, 0xD0, 0x0B, 0x15, 0x08,
    0x62, 0xD2, 0x0B, 0x45, 0x54, 0x32, 0xD2, 0x0C, 0x45, 0x55,
    0x62, 0xD2, 0x0B, 0x43, 0x54, 0x42, 0xD2, 0x0C, 0x46, 0x55,
    0xE2, 0xCF, 0x0C, 0x54, 0x14, 0x12, 0xD0, 0x0B, 0x14, 0x08,
    0xC2, 0xD3, 0x0B, 0x34, 0x65, 0xB2, 0xD3, 0x0B, 0x95, 0x65,
};


String getPumpFlow(uint8_t* dataPtr)
{
    int pumpFlowInt = (int)dataPtr[1];
    float pumpFlowFract = (((float)dataPtr[0] - 1) / 256);
    float pumpFlow = pumpFlowInt + pumpFlowFract;
    return String(pumpFlow, 2);
}

String getModel(uint8_t* dataPtr)
{
    for (int i = 0 ; i < sizeof(_knownModels) / sizeof(_knownModels[0]) ; i++)
    {
        if (memcmp_P(dataPtr, _knownModels[i], 10) == 0)
        {
            return String(i);
        }
    }
    return F("-1");
}

String getErrorInfo(uint8_t* dataPtr)
{
    int errorType = (int)(dataPtr[0]);
    int errorNumber = ((int)(dataPtr[1])) - 17;
    char errorString[8];
    switch (errorType) 
    {
        case 177: //B1=F type error
            sprintf(errorString, "F%02X", errorNumber);
            break;
        case 161: //A1=H type error
            sprintf(errorString, "H%02X", errorNumber);
            break;
        default:
            return F("No error");
    }
    return String(errorString);
}

String getWordValue(uint8_t* dataPtr)
{
    return String(word(dataPtr[1], dataPtr[0]) - 1);
}

String getBit1and2(uint8_t* dataPtr)
{
    return String((*dataPtr >> 6) - 1);
}

String getBit3and4(uint8_t* dataPtr)
{
    return String(((*dataPtr >> 4) & 0b11) - 1);
}

String getBit5and6(uint8_t* dataPtr)
{
    return String(((*dataPtr >> 2) & 0b11) - 1);
}

String getBit7and8(uint8_t* dataPtr)
{
    return String((*dataPtr & 0b11) - 1);
}

String getBit3and4and5(uint8_t* dataPtr)
{
    return String(((*dataPtr >> 3) & 0b111) - 1);
}

String getLeft5bits(uint8_t* dataPtr)
{
    return String((*dataPtr >> 3) - 1);
}

String getRight3bits(uint8_t* dataPtr)
{
    return String((*dataPtr & 0b111) - 1);
}

String getIntMinus1(uint8_t* dataPtr)
{
    int value = (int)*dataPtr - 1;
    return (String)value;
}

String getIntMinus128(uint8_t* dataPtr)
{
    int value = (int)*dataPtr - 128;
    return (String)value;
}

String getIntMinus1Div5(uint8_t* dataPtr)
{
    return String((((float)*dataPtr - 1) / 5), 1);
}

String getIntMinus1Times10(uint8_t* dataPtr)
{
    int value = (int)*dataPtr - 1;
    return (String)(value * 10);
}

String getIntMinus1Times50(uint8_t* dataPtr)
{
    int value = (int)*dataPtr - 1;
    return (String)(value * 50);
}

String getOpMode(uint8_t* dataPtr)
{
    switch ((int)(*dataPtr & 0b111111))
    {
        case 18:
            return "0";
        case 19:
            return "1";
        case 25:
            return "2";
        case 33:
            return "3";
        case 34:
            return "4";
        case 35:
            return "5";
        case 41:
            return "6";
        case 26:
            return "7";
        case 42:
            return "8";
        default:
            return "-1";
    }
}

String getEnergy(uint8_t* dataPtr)
{
    int value = ((int)*dataPtr - 1) * 200;
    return (String)value;
}

String getSolarDeltaT(uint8_t* dataPtr)
{
    int solarTemp = static_cast<int>(dataPtr[150]) - 128;
    int bufferTemp = static_cast<int>(dataPtr[149]) - 128;
    return String(solarTemp - bufferTemp);
}

String getPower(uint8_t* dataPtr)
{
    float current = float(*dataPtr - 1) / 5;
    float powerKW = current * 230 / 1000;
    return String(powerKW, 1);
}

String getHeatPower(uint8_t* dataPtr)
{
    float pumpFlow = getPumpFlow(dataPtr + 169).toFloat();
    if (pumpFlow < 0.5) return F("0.0");
    int inletTemp = static_cast<int>(dataPtr[143]) - 128;
    int outletTemp = static_cast<int>(dataPtr[144]) - 128;
    float heatPowerKW = 4.186 * (pumpFlow / 60) * std::max(outletTemp - inletTemp, 0);
    return String(heatPowerKW, 1);
}


TopicDesc _topicDescriptors[] PROGMEM = 
{
    { "Heatpump_State", 4, getBit7and8, OffOn },
    { "Pump_Flow", 169, getPumpFlow, LitersPerMin },
    { "Force_DHW_State", 4, getBit1and2, DisabledEnabled },
    { "Quiet_Mode_Schedule", 7, getBit1and2, DisabledEnabled },
    { "Operating_Mode_State", 6, getOpMode, OpModeDesc },
    { "Main_Inlet_Temp", 143, getIntMinus128, Celsius },
    { "Main_Outlet_Temp", 144, getIntMinus128, Celsius },
    { "Main_Target_Temp", 153, getIntMinus128, Celsius },
    { "Compressor_Freq", 166, getIntMinus1, Hertz },
    { "DHW_Target_Temp", 42, getIntMinus128, Celsius },
    { "DHW_Temp", 141, getIntMinus128, Celsius },
    { "Operations_Hours", 182, getWordValue, Hours },
    { "Operations_Counter", 179, getWordValue, Counter },
    { "Main_Schedule_State", 5, getBit1and2, DisabledEnabled },
    { "Outside_Temp", 142, getIntMinus128, Celsius },
    { "Heat_Energy_Production", 194, getEnergy, Watt },
    { "Heat_Energy_Consumption", 193, getEnergy, Watt },
    { "Powerful_Mode_Time", 7, getRight3bits, Powerfulmode },
    { "Quiet_Mode_Level", 7, getBit3and4and5, Quietmode },
    { "Holiday_Mode_State", 5, getBit3and4, HolidayState },
    { "ThreeWay_Valve_State", 111, getBit7and8, Valve },
    { "Outside_Pipe_Temp", 158, getIntMinus128, Celsius },
    { "DHW_Heat_Delta", 99, getIntMinus128, Kelvin },
    { "Heat_Delta", 84, getIntMinus128, Kelvin },
    { "Cool_Delta", 94, getIntMinus128, Kelvin },
    { "DHW_Holiday_Shift_Temp", 44, getIntMinus128, Kelvin },
    { "Defrosting_State", 111, getBit5and6, DisabledEnabled },
    { "Z1_Heat_Request_Temp", 38, getIntMinus128, Celsius },
    { "Z1_Cool_Request_Temp", 39, getIntMinus128, Celsius },
    { "Z1_Heat_Curve_Target_High_Temp", 75, getIntMinus128, Celsius },
    { "Z1_Heat_Curve_Target_Low_Temp", 76, getIntMinus128, Celsius },
    { "Z1_Heat_Curve_Outside_High_Temp", 78, getIntMinus128, Celsius },
    { "Z1_Heat_Curve_Outside_Low_Temp", 77, getIntMinus128, Celsius },
    { "Room_Thermostat_Temp", 156, getIntMinus128, Celsius },
    { "Z2_Heat_Request_Temp", 40, getIntMinus128, Celsius },
    { "Z2_Cool_Request_Temp", 41, getIntMinus128, Celsius },
    { "Z1_Water_Temp", 145, getIntMinus128, Celsius },
    { "Z2_Water_Temp", 146, getIntMinus128, Celsius },
    { "Cool_Energy_Production", 196, getEnergy, Watt },
    { "Cool_Energy_Consumption", 195, getEnergy, Watt },
    { "DHW_Energy_Production", 198, getEnergy, Watt },
    { "DHW_Energy_Consumption", 197, getEnergy, Watt },
    { "Z1_Water_Target_Temp", 147, getIntMinus128, Celsius },
    { "Z2_Water_Target_Temp", 148, getIntMinus128, Celsius },
    { "Error", 113, getErrorInfo, ErrorState },
    { "Room_Holiday_Shift_Temp", 43, getIntMinus128, Kelvin },
    { "Buffer_Temp", 149, getIntMinus128, Celsius },
    { "Solar_Temp", 150, getIntMinus128, Celsius },
    { "Pool_Temp", 151, getIntMinus128, Celsius },
    { "Main_Hex_Outlet_Temp", 154, getIntMinus128, Celsius },
    { "Discharge_Temp", 155, getIntMinus128, Celsius },
    { "Inside_Pipe_Temp", 157, getIntMinus128, Celsius },
    { "Defrost_Temp", 159, getIntMinus128, Celsius },
    { "Eva_Outlet_Temp", 160, getIntMinus128, Celsius },
    { "Bypass_Outlet_Temp", 161, getIntMinus128, Celsius },
    { "Ipm_Temp", 162, getIntMinus128, Celsius },
    { "Z1_Temp", 139, getIntMinus128, Celsius },
    { "Z2_Temp", 140, getIntMinus128, Celsius },
    { "DHW_Heater_State", 9, getBit5and6, BlockedFree },
    { "Room_Heater_State", 9, getBit7and8, BlockedFree },
    { "Internal_Heater_State", 112, getBit7and8, InactiveActive },
    { "External_Heater_State", 112, getBit5and6, InactiveActive },
    { "Fan1_Motor_Speed", 173, getIntMinus1Times10, RotationsPerMin },
    { "Fan2_Motor_Speed", 174, getIntMinus1Times10, RotationsPerMin },
    { "High_Pressure", 163, getIntMinus1Div5, Pressure },
    { "Pump_Speed", 171, getIntMinus1Times50, RotationsPerMin },
    { "Low_Pressure", 164, getIntMinus1, Pressure },
    { "Compressor_Current", 165, getIntMinus1Div5, Ampere },
    { "Force_Heater_State", 5, getBit5and6, InactiveActive },
    { "Sterilization_State", 117, getBit5and6, InactiveActive },
    { "Sterilization_Temp", 100, getIntMinus128, Celsius },
    { "Sterilization_Max_Time", 101, getIntMinus1, Minutes },
    { "Z1_Cool_Curve_Target_High_Temp", 86, getIntMinus128, Celsius },
    { "Z1_Cool_Curve_Target_Low_Temp", 87, getIntMinus128, Celsius },
    { "Z1_Cool_Curve_Outside_High_Temp", 89, getIntMinus128, Celsius },
    { "Z1_Cool_Curve_Outside_Low_Temp", 88, getIntMinus128, Celsius },
    { "Heating_Mode", 28, getBit7and8, HeatCoolModeDesc },
    { "Heating_Off_Outdoor_Temp", 83, getIntMinus128, Celsius },
    { "Heater_On_Outdoor_Temp", 85, getIntMinus128, Celsius },
    { "Heat_To_Cool_Temp", 95, getIntMinus128, Celsius },
    { "Cool_To_Heat_Temp", 96, getIntMinus128, Celsius },
    { "Cooling_Mode", 28, getBit5and6, HeatCoolModeDesc },
    { "Z2_Heat_Curve_Target_High_Temp", 79, getIntMinus128, Celsius },
    { "Z2_Heat_Curve_Target_Low_Temp", 80, getIntMinus128, Celsius },
    { "Z2_Heat_Curve_Outside_High_Temp", 82, getIntMinus128, Celsius },
    { "Z2_Heat_Curve_Outside_Low_Temp", 81, getIntMinus128, Celsius },
    { "Z2_Cool_Curve_Target_High_Temp", 90, getIntMinus128, Celsius },
    { "Z2_Cool_Curve_Target_Low_Temp", 91, getIntMinus128, Celsius },
    { "Z2_Cool_Curve_Outside_High_Temp", 93, getIntMinus128, Celsius },
    { "Z2_Cool_Curve_Outside_Low_Temp", 92, getIntMinus128, Celsius },
    { "Room_Heater_Operations_Hours", 185, getWordValue, Hours },
    { "DHW_Heater_Operations_Hours", 188, getWordValue, Hours },
    { "Heat_Pump_Model", 129, getModel, Model },
    { "Pump_Duty", 172, getIntMinus1, Duty },
    { "Zones_State", 6, getBit1and2, ZonesState },
    { "Max_Pump_Duty", 45, getIntMinus1, Duty },
    { "Heater_Delay_Time", 104, getIntMinus1, Minutes },
    { "Heater_Start_Delta", 105, getIntMinus128, Kelvin },
    { "Heater_Stop_Delta", 106, getIntMinus128, Kelvin },
    { "Buffer_Installed", 24, getBit5and6, DisabledEnabled },
    { "DHW_Installed", 24, getBit7and8, DisabledEnabled },
    { "Solar_Mode", 24, getBit3and4, SolarModeDesc },
    { "Solar_On_Delta", 61, getIntMinus128, Kelvin },
    { "Solar_Off_Delta", 62, getIntMinus128, Kelvin },
    { "Solar_Frost_Protection", 63, getIntMinus128, Celsius },
    { "Solar_High_Limit", 64, getIntMinus128, Celsius },
    { "Solar_DeltaT", 0, getSolarDeltaT, Celsius },
    { "Compressor_Power", 165, getPower, KW },
    { "Heat_Power", 0, getHeatPower, KW },
};


String Topic::getId()
{
    String result = F("TOP");
    result += static_cast<int>(_topicId);
    return result;
}


String Topic::getName()
{
    return String(FPSTR(_descriptor.name));
}


String Topic::getDescription()
{
    char descriptionMapType[8];
    strncpy_P(descriptionMapType, _descriptor.descriptionMapping[0], sizeof(descriptionMapType));

    const __FlashStringHelper* description;
    if (strcmp(descriptionMapType, "value") == 0)
    {
        // Fixed value (typically unit of measure)
        description = FPSTR(_descriptor.descriptionMapping[1]);
    }
    else 
    {
        // Map topic value to a description
        int topicValue = _value.toInt();
        int maxValue = atoi(descriptionMapType);
        if ((topicValue < 0) || (topicValue > maxValue))
            description = F("???");
        else
            description = FPSTR(_descriptor.descriptionMapping[topicValue + 1]);
    }

    return String(description);
}


// Constructor
Aquarea::Aquarea()
{
#ifdef DEBUG_ESP_PORT
    _debugOutputOnSerial = (&DEBUG_ESP_PORT == &Serial);
#endif

    memset(_queryData, 0, AQUAREA_COMMAND_DATA_SIZE);
    _queryData[0] = 0x01;
    _queryData[1] = 0x10;
}


void Aquarea::resetPacketStats()
{
    _validPackets = 0;
    _repairedPackets = 0;
    _invalidPackets = 0;
}


bool Aquarea::begin()
{
    Tracer tracer(F("Aquarea::begin"));

    if (_debugOutputOnSerial) 
        Serial.println(F("WARNING: DEBUG_ESP_PORT is set to Serial. Not switching Serial; heatpump communication won't work."));
    else
    {
        // Configure Serial for Aquarea connection (CN-CNT) 
        Serial.flush();
        Serial.end();
        Serial.begin(9600, SERIAL_8E1);
        Serial.flush();
        Serial.swap(); // Use GPIO13/D7 (RX) and GPIO15/D8 (TX)

        // Configure original TX/RX pins (GPIO1/3) for later use
        pinMode(1, FUNCTION_3);
        pinMode(3, FUNCTION_3);

        // Connect Aquarea RX with GPIO15 (TX)
        pinMode(5, OUTPUT);
        digitalWrite(5, HIGH);
    }

    Serial.setRxBufferSize(512);
    Serial.setTimeout(500);
    return true;
}


uint8_t Aquarea::checkSum(uint8_t magic, uint8_t dataSize, uint8_t* dataPtr)
{
    uint8_t sum = magic + dataSize;
    for (uint8_t i = 0; i < dataSize; i++) sum += dataPtr[i]; 
    return (sum ^ 0xFF) + 01;
}


bool Aquarea::validateCheckSum()
{
    int packetSize = _data[1] + 3;

    uint8_t sum = 0;
    for (int i = 0; i < packetSize; i++)
        sum += _data[i];

    if (sum == 0)
    {
        _validPackets++;
        return true;
    }
    else
    {
        _invalidPackets++;
        _lastError = F("Checksum error: sum = 0x");
        _lastError += String(sum, 16);
        return false;           
    }
}


bool Aquarea::sendCommand(uint8_t magic, uint8_t dataSize, uint8_t* dataPtr)
{
    if (_commandSentMillis != 0)
    {
        uint32_t timeoutMillis = _commandSentMillis + 2000;
        if (millis() >= timeoutMillis)
            TRACE(F("No response received for earlier command.\n"));
        else
        {
            TRACE(F("Awaiting response of earlier command...\n"));
            while ((_commandSentMillis != 0)  && (millis() < timeoutMillis))
            {
                delay(100);
                if (Serial.available()) readPacket();
            }
            if (_commandSentMillis != 0)
            {
                TRACE(F("Timeout after %u ms.\n"), millis() - _commandSentMillis);
            }
        }
        _commandSentMillis = 0;
    }

    uint8_t checkSum = Aquarea::checkSum(magic, dataSize, dataPtr); 

    int bytesSent = Serial.write(magic);
    bytesSent += Serial.write(dataSize); 
    bytesSent += Serial.write(dataPtr, dataSize);
    bytesSent += Serial.write(checkSum);

    TRACE(
        F("\nSent %d bytes to Aquarea. Magic: 0x%02X. Data size: %d. Checksum: 0x%02X\n"),
        bytesSent,
        static_cast<int>(magic),
        static_cast<int>(dataSize),
        static_cast<int>(checkSum));

    _commandSentMillis = millis();
    return (bytesSent == dataSize + 3);
}


bool Aquarea::sendQuery()
{
    Tracer tracer(F("Aquarea::sendQuery"));

    return sendCommand(AQUAREA_QUERY_MAGIC, AQUAREA_COMMAND_DATA_SIZE, _queryData);
}


bool Aquarea::setPump(bool pumpOn)
{
    Tracer tracer(F("Aquarea::setPump"), pumpOn ? "on" : "off");

    memset(_commandData, 0, AQUAREA_COMMAND_DATA_SIZE);
    _commandData[0] = 0x01;
    _commandData[1] = 0x10;
    _commandData[2] = pumpOn ? 32 : 16;

    return sendCommand(0xF1, AQUAREA_COMMAND_DATA_SIZE, _commandData);
}


const char* formatPacketInfo(uint8_t magic, uint8_t dataSize, size_t readBytes)
{
    static char result[64];
    snprintf(
        result,
        sizeof(result),
        "Magic: 0x%02X. Data size: %d. Received: %u.",
        static_cast<int>(magic),
        static_cast<int>(dataSize),
        readBytes);
    return result;
}


bool Aquarea::readPacket()
{
    Tracer tracer(F("Aquarea::readPacket"));

    PacketHeader header;
    if (readBytes((uint8_t*)&header, sizeof(header)) != sizeof(header))
    {
        _lastError = F("Timeout reading packet header");
        _invalidPackets++;
        return false;
    }

    // Some kind of response is received; allow next command to be sent.
    _commandSentMillis = 0; 

    size_t bytesRead;
    if (_debugOutputOnSerial && (header.magic == 't'))
    {
        // Test packet for debug purposes
        bytesRead = readTestData(header);
        if (bytesRead < 0) 
        {
            _invalidPackets++;
            return false;
        }
    }
    else
    {
        uint8_t* dataPtr = (header.magic == AQUAREA_RESPONSE_MAGIC) && (header.dataSize == AQUAREA_RESPONSE_DATA_SIZE)
            ? _data
            : _invalidData;

        memset(dataPtr, 0xEE, DATA_BUFFER_SIZE); // Mark "Empty" bytes in hex dump
        dataPtr[0] = header.magic;
        dataPtr[1] = header.dataSize;

        // Always try to read more bytes than expected to ensure the RX buffer is flushed
        // Timeout is set to 500 ms, so this won't block too long.
        bytesRead = readBytes(dataPtr + 2, DATA_BUFFER_SIZE - 2);
    }

    TRACE(F("Received packet. %s\n"), formatPacketInfo(header.magic, header.dataSize, bytesRead));

    if ((header.magic == AQUAREA_RESPONSE_MAGIC) || (header.dataSize == AQUAREA_RESPONSE_DATA_SIZE))
    {
        if (bytesRead != header.dataSize + 1)
        {
            _lastError = formatPacketInfo(header.magic, header.dataSize, bytesRead);
            _invalidPackets++;
            return false;
        }
    }
    else
    {
        // Typical packet mutilations which can be repaired:
        if ((header.dataSize == 190) && (bytesRead == 200) & (_invalidData[2] == 0x10))
        {
            TRACE(F("Repairing packet.\n"));
            _data[0] = AQUAREA_RESPONSE_MAGIC;
            _data[1] = AQUAREA_RESPONSE_DATA_SIZE;
            _data[2] = 1;
            memcpy(_data + 3, _invalidData + 2, 200);
            _repairedPackets++;
        }
        else
        {
            _lastError = formatPacketInfo(header.magic, header.dataSize, bytesRead);
            _invalidPackets++;
            return false;
        }
    }

    return validateCheckSum();  
}


int Aquarea::readTestData(PacketHeader& header)
{
    char testCommand = header.dataSize;
    TRACE(F("Aquarea::readTestData(). testCommand: '%c'\n"), testCommand);

    size_t bytesRead = AQUAREA_RESPONSE_DATA_SIZE + 1;
    if (testCommand == 'o')
    {
        TRACE(F("Setting topic value...\n"));
        bool success = setTopicValue();
        if (!success) return -2;
        header.magic = _data[0];
        header.dataSize = _data[1];
    }
    else if (testCommand == ' ')
    {
        TRACE(F("Receiving packet data in hexdump form...\n"));

        if (readHexBytes((uint8_t*)&header, sizeof(header)) != sizeof(header))
        {
            _lastError = F("Timeout reading packet header");
            return -1;
        }

        uint8_t* dataPtr = (header.magic == AQUAREA_RESPONSE_MAGIC) && (header.dataSize == AQUAREA_RESPONSE_DATA_SIZE)
            ? _data
            : _invalidData;

        memset(dataPtr, 0xEE, DATA_BUFFER_SIZE); // Mark "Empty" bytes in hex dump
        dataPtr[0] = header.magic;
        dataPtr[1] = header.dataSize;

        // Always try to read more bytes than expected to ensure the RX buffer is flushed
        // Timeout is set to 500 ms, so this won't block too long.
        bytesRead = readHexBytes(dataPtr + 2, DATA_BUFFER_SIZE - 2);
    }
    else
        TRACE(F("Repeating last packet.\n"));

    return bytesRead; 
}


bool Aquarea::setTopicValue()
{
    String topicName = Serial.readStringUntil('=');
    topicName.trim();

    uint8_t value;
    if (readHexBytes(&value, 1) != 1)
    {
        _lastError = F("Timeout reading byte value");
        return false;
    }

    bool knownTopic = false;
    TopicDesc topicDescriptor;
    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        if (strcmp_P(topicName.c_str(), _topicDescriptors[i].name) == 0)
        {
            memcpy_P(&topicDescriptor, &_topicDescriptors[i], sizeof(TopicDesc));
            knownTopic = true;
            break;
        }
    }
    if (!knownTopic)
    {
        _lastError = F("Unknown Topic: ");
        _lastError += topicName;
        return false;
    }

    _data[0] = AQUAREA_RESPONSE_MAGIC;
    _data[1] = AQUAREA_RESPONSE_DATA_SIZE;
    _data[2] = 0x01;
    _data[3] = 0x10;
    _data[topicDescriptor.index] = value;

    uint8_t checkSum = Aquarea::checkSum(_data[0], _data[1], _data + 2);
    _data[AQUAREA_RESPONSE_DATA_SIZE + 2] = checkSum;

    TRACE(
        F("_data[%d] = %d. Checksum: 0x%02X\n"),
        topicDescriptor.index,
        static_cast<int>(value),
        checkSum);
        
    return true;
}


size_t Aquarea::readBytes(uint8_t* bufferPtr, size_t count)
{
    size_t bytesRead = Serial.readBytes(bufferPtr, count);
    if (bytesRead != count)
        TRACE(F("Timeout reading %d bytes. %d bytes read.\n"), count, bytesRead);
    return bytesRead;
}


size_t Aquarea::readHexBytes(uint8_t* bufferPtr, size_t count)
{
    char hexDigits[3];
    hexDigits[2] = 0;

    for (int i = 0; i < count; i++)
    {
        if (Serial.readBytes((uint8_t*)hexDigits, 2) != 2)
        {
            TRACE(F("Timeout reading %d hex bytes. %d hex bytes read.\n"), count, i);
            return i;
        }

        uint32_t byte;
        sscanf(hexDigits, "%X", &byte);
        bufferPtr[i] = byte; 
    }

    return count;
}


void Aquarea::writeHexDump(Print& printTo, bool showInvalidData)
{
    uint8_t* dataPtr;
    int length;
    if (showInvalidData)
    {
        dataPtr = _invalidData;
        length = DATA_BUFFER_SIZE;
    }
    else
    {
        dataPtr = _data;
        length = std::min(dataPtr[1] + 3, DATA_BUFFER_SIZE);
    }

    for (int row = 0; row < length; row += 16)
    {
        for (int col = 0; col < 16; col++)
        {
            int index = row + col;
            if (index == length) break; 
            printTo.printf("%02X ", dataPtr[index]);
            if (col == 7)
                printTo.print(" ");
        }
        printTo.println();
    }
}


Topic Aquarea::getTopic(TopicId id)
{
    TopicDesc topicDescriptor;
    memcpy_P(&topicDescriptor, &_topicDescriptors[static_cast<int>(id)], sizeof(TopicDesc));

    String topicValue = topicDescriptor.conversion(_data + topicDescriptor.index);

    return Topic(id, topicValue, topicDescriptor);
}


std::vector<TopicId> Aquarea::getAllTopicIds()
{
    std::vector<TopicId> result;

    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        TopicId topicId = static_cast<TopicId>(i);
        result.push_back(topicId);
    }

    return result;
}
