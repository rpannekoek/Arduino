#ifndef OTGW_H
#define OTGW_H

#include <c_types.h>
#include <WString.h>
#include <Stream.h>


typedef enum 
{
    ReadData = 0,
    WriteData = 1,
    InvalidDate = 2,
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
    TRoot = 24,
    TBoiler = 25
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


typedef struct 
{
    String message;
    OpenThermGatewayDirection direction;
    OpenThermMsgType msgType;
    OpenThermDataId dataId;
    uint16_t dataValue;
} OpenThermGatewayMessage;


class OpenThermGateway
{
    public:
        uint32_t errors[5];

        OpenThermGateway(Stream& serial);

        void reset();
        OpenThermGatewayMessage readMessage();
        bool sendCommand(const char* cmd, const char* value);

    protected:
        Stream& _serial;
};


#endif