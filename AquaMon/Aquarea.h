#ifndef AQUAREA_H
#define AQUAREA_H

#define DATA_BUFFER_SIZE 256

enum struct TopicId
{
    Heatpump_State = 0,
    Pump_Flow = 1,
    Force_DHW_State = 2, 
    Quiet_Mode_Schedule = 3,
    Operating_Mode_State = 4,
    Main_Inlet_Temp = 5,
    Main_Outlet_Temp = 6,
    Main_Target_Temp = 7,
    Compressor_Freq = 8,
    DHW_Target_Temp = 9,
    DHW_Temp = 10,
    Operations_Hours = 11,
    Operations_Counter = 12,
    Main_Schedule_State = 13,
    Outside_Temp = 14,
    Heat_Energy_Production = 15,
    Heat_Energy_Consumption = 16,
    Powerful_Mode_Time = 17,
    Quiet_Mode_Level = 18,
    Holiday_Mode_State = 19,
    ThreeWay_Valve_State = 20,
    Outside_Pipe_Temp = 21,
    DHW_Heat_Delta = 22,
    Heat_Delta = 23,
    Cool_Delta = 24,
    DHW_Holiday_Shift_Temp = 25,
    Defrosting_State = 26,
    Z1_Heat_Request_Temp = 27,
    Z1_Cool_Request_Temp = 28,
    Z1_Heat_Curve_Target_High_Temp = 29,
    Z1_Heat_Curve_Target_Low_Temp = 30,
    Z1_Heat_Curve_Outside_High_Temp = 31,
    Z1_Heat_Curve_Outside_Low_Temp = 32,
    Room_Thermostat_Temp = 33,
    Z2_Heat_Request_Temp = 34,
    Z2_Cool_Request_Temp = 35,
    Z1_Water_Temp = 36,
    Z2_Water_Temp = 37,
    Cool_Energy_Production = 38,
    Cool_Energy_Consumption = 39,
    DHW_Energy_Production = 40,
    DHW_Energy_Consumption = 41,
    Z1_Water_Target_Temp = 42,
    Z2_Water_Target_Temp = 43,
    Error = 44,
    Room_Holiday_Shift_Temp = 45,
    Buffer_Temp = 46,
    Solar_Temp = 47,
    Pool_Temp = 48,
    Main_Hex_Outlet_Temp = 49,
    Discharge_Temp = 50,
    Inside_Pipe_Temp = 51,
    Defrost_Temp = 52,
    Eva_Outlet_Temp = 53,
    Bypass_Outlet_Temp = 54,
    Ipm_Temp = 55,
    Z1_Temp = 56,
    Z2_Temp = 57,
    DHW_Heater_State = 58,
    Room_Heater_State = 59,
    Internal_Heater_State = 60,
    External_Heater_State = 61,
    Fan1_Motor_Speed = 62,
    Fan2_Motor_Speed = 63,
    High_Pressure = 64,
    Pump_Speed = 65,
    Low_Pressure = 66,
    Compressor_Current = 67,
    Force_Heater_State = 68,
    Sterilization_State = 69,
    Sterilization_Temp = 70,
    Sterilization_Max_Time = 71,
    Z1_Cool_Curve_Target_High_Temp = 72,
    Z1_Cool_Curve_Target_Low_Temp = 73,
    Z1_Cool_Curve_Outside_High_Temp = 74,
    Z1_Cool_Curve_Outside_Low_Temp = 75,
    Heating_Mode = 76,
    Heating_Off_Outdoor_Temp = 77,
    Heater_On_Outdoor_Temp = 78,
    Heat_To_Cool_Temp = 79,
    Cool_To_Heat_Temp = 80,
    Cooling_Mode = 81,
    Z2_Heat_Curve_Target_High_Temp = 82,
    Z2_Heat_Curve_Target_Low_Temp = 83,
    Z2_Heat_Curve_Outside_High_Temp = 84,
    Z2_Heat_Curve_Outside_Low_Temp = 85,
    Z2_Cool_Curve_Target_High_Temp = 86,
    Z2_Cool_Curve_Target_Low_Temp = 87,
    Z2_Cool_Curve_Outside_High_Temp = 88,
    Z2_Cool_Curve_Outside_Low_Temp = 89,
    Room_Heater_Operations_Hours = 90,
    DHW_Heater_Operations_Hours = 91,
    Heat_Pump_Model = 92,
    Pump_Duty = 93,
    Zones_State = 94,
    Max_Pump_Duty = 95,
    Heater_Delay_Time = 96,
    Heater_Start_Delta = 97,
    Heater_Stop_Delta = 98,
    Buffer_Installed = 99,
    DHW_Installed = 100,
    Solar_Mode = 101,
    Solar_On_Delta = 102,
    Solar_Off_Delta = 103,
    Solar_Frost_Protection = 104,
    Solar_High_Limit = 105,
    Solar_DeltaT = 106,
    Compressor_Power = 107,
    Heat_Power = 108
};


struct __attribute__ ((packed)) PacketHeader
{
    uint8_t magic;
    uint8_t dataSize;
};


struct TopicDesc
{
    PGM_P name;
    uint8_t index;
    String (*conversion)(uint8_t*);
    PGM_P* descriptionMapping;
};


class Topic
{
    public:
        Topic(TopicId topicId, String& value, TopicDesc& descriptor)
            : _topicId(topicId), _value(value), _descriptor(descriptor)  {}

        TopicId inline getTopicId()
        {
            return _topicId;
        }

        String inline getValue()
        {
            return _value;
        }

        String getId();
        String getName();
        String getDescription();

    private:
        TopicId _topicId;
        String _value;
        TopicDesc _descriptor;
};


class Aquarea
{
    public:
        // Constructor
        Aquarea();

        void setZone1Offset(float offset)
        {
            _zone1Offset = offset;
        }

        String inline getLastError()
        {
            return _lastError;
        }

        uint32_t inline getValidPackets()
        {
            return _validPackets;
        }

        uint32_t inline getInvalidPackets()
        {
            return _invalidPackets;
        }

        uint32_t inline getRepairedPackets()
        {
            return _repairedPackets;
        }

        float inline getPacketErrorRatio()
        {
            return float(_invalidPackets) / (_validPackets + _invalidPackets);
        }

        static std::vector<TopicId> getAllTopicIds();

        Topic getTopic(TopicId id);

        bool begin();
        bool sendQuery();
        bool setPump(bool pumpOn);
        bool readPacket();
        void writeHexDump(Print& printTo, bool unknownData);
        void resetPacketStats();

    private:
        uint8_t _data[DATA_BUFFER_SIZE];
        uint8_t _invalidData[DATA_BUFFER_SIZE];
        uint32_t _validPackets = 0;
        uint32_t _repairedPackets = 0;
        uint32_t _invalidPackets = 0;
        String _lastError;
        uint32_t _commandSentMillis = 0;
        bool _debugOutputOnSerial = false;
        static float _zone1Offset;

        static TopicDesc getTopicDescriptor(TopicId topicId);
        static uint8_t checkSum(uint8_t magic, uint8_t dataSize, uint8_t* dataPtr);
        bool validateCheckSum();
        bool sendCommand(uint8_t magic, uint8_t dataSize, uint8_t* dataPtr);
        int readTestData(PacketHeader& header);
        bool setTopicValue();
        static size_t readBytes(uint8_t* bufferPtr, size_t count);
        static size_t readHexBytes(uint8_t* bufferPtr, size_t count);
        friend String getZone1Temp(uint8_t*);
};

#endif