#ifndef OTGW_H
#define OTGW_H

#include <stdint.h>
#include <WString.h>
#include <Stream.h>


enum struct OpenThermMsgType
{
    ReadData = 0,
    WriteData = 1,
    InvalidData = 2,
    ReadAck = 4,
    WriteAck = 5,
    DataInvalid = 6,
    UnknownDataId = 7
};


enum OpenThermDataId // Unscoped enum so it can be used as array index without casting
{
    Status = 0,
    TSet = 1,
    SlaveFault = 5,
    MaxRelModulation = 14,
    TRoomSet = 16,
    TRoom = 24,
    TBoiler = 25,
    TOutside = 27,
    TReturn = 28,
    MaxTSet = 57,
    BoilerBurnerStarts = 116,
    BoilerBurnerHours = 120,
    BoilerDHWBurnerHours = 123
};


enum OpenThermStatus // Bitflags
{
    SlaveCHMode = 0x2,
    SlaveDHWMode = 0x4,
    SlaveFlame = 0x8,
    MasterCHEnable = 0x100,
    MasterDHWEnable = 0x200,
} ;


enum struct OpenThermGatewayDirection
{
    FromThermostat,
    FromBoiler,
    ToThermostat,
    ToBoiler,
    Error,
    Unexpected
};


struct OpenThermGatewayMessage
{
    String message;
    OpenThermGatewayDirection direction;
    OpenThermMsgType msgType;
    OpenThermDataId dataId;
    uint16_t dataValue;
};


class OpenThermGateway
{
    public:
        uint32_t errors[5];
        uint32_t resets;

        OpenThermGateway(Stream& serial, uint8_t resetPin);

        void reset();
        void feedWatchdog();
        OpenThermGatewayMessage readMessage();
        bool sendCommand(const char* cmd, const char* value, char* response = nullptr, size_t bufferSize = 2);

        static const char* getMasterStatus(uint16_t dataValue);
        static const char* getSlaveStatus(uint16_t dataValue);
        static const char* getFaultFlags(uint16_t dataValue);
        static float getDecimal(uint16_t dataValue);

    protected:
        Stream& _serial;
        uint8_t _resetPin;
};


#endif