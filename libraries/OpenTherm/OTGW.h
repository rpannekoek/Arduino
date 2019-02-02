#ifndef OTGW_H
#define OTGW_H

#include <c_types.h>
#include <WString.h>
#include <Stream.h>


typedef enum 
{
    ReadData = 0,
    WriteData = 1,
    InvalidData = 2,
    ReadAck = 4,
    WriteAck = 5,
    DataInvalid = 6,
    UnknownDataId = 7
} OpenThermMsgType;


typedef enum
{
    Status = 0,
    TSet = 1,
    MaxRelModulation = 14,
    TRoomSet = 16,
    TRoom = 24,
    TBoiler = 25,
    MaxTSet = 57
} OpenThermDataId;


typedef enum 
{
    SlaveCHMode = 0x2,
    SlaveDHWMode = 0x4,
    MasterCHEnable = 0x100,
} OpenThermStatus;


typedef enum
{
    FromThermostat,
    FromBoiler,
    ToThermostat,
    ToBoiler,
    Error
} OpenThermGatewayDirection;


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

        OpenThermGateway(Stream& serial, uint8_t resetPin);

        void reset();
        void feedWatchdog();
        OpenThermGatewayMessage readMessage();
        bool sendCommand(const char* cmd, const char* value);

    protected:
        Stream& _serial;
        uint8_t _resetPin;
};


#endif