#include <OTGW.h>
#include <Tracer.h>
#include <Arduino.h>
#include <Wire.h>

#define OTGW_MESSAGE_SIZE 11
#define MAX_RETRIES 5
#define WATCHDOG_I2C_ADDRESS 38


OpenThermGateway::OpenThermGateway(Stream& serial, uint8_t resetPin)
    : _serial(serial), _resetPin(resetPin)
{
    memset(errors, 0, sizeof(errors));
    Wire.begin();
}


void OpenThermGateway::reset()
{
    Tracer tracer("OpenThermGateway::reset");

    pinMode(_resetPin, OUTPUT);
    digitalWrite(_resetPin, LOW);
    delay(100);
    digitalWrite(_resetPin, HIGH);
    pinMode(_resetPin, INPUT_PULLUP);
}


void OpenThermGateway::feedWatchdog()
{
    Tracer tracer("OpenThermGateway::feedWatchdog");

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(0xA5);
    Wire.endTransmission();
}


OpenThermGatewayMessage OpenThermGateway::readMessage()
{
    Tracer tracer("OpenThermGateway::readMessage");

    OpenThermGatewayMessage result;

    char otgwMessage[OTGW_MESSAGE_SIZE + 1];
    size_t bytesRead = _serial.readBytesUntil('\n',  otgwMessage, OTGW_MESSAGE_SIZE);
    if (bytesRead == 0) 
    {
        result.message = "[Timeout]";
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }
    otgwMessage[bytesRead] = 0;
    TRACE("Message from OTGW: %s\n", otgwMessage);
    result.message = otgwMessage;

    // Check for gateway errors
    unsigned int errorCode;
    if (strncmp(otgwMessage, "Error", 5) == 0)
    {
        if ((sscanf(otgwMessage + 6, "%x", &errorCode) == 1) && (errorCode > 0) && (errorCode < 5))
            errors[errorCode]++;
        else
            errors[0]++;
        result.direction = OpenThermGatewayDirection::Error;
        return result;
    }

    // Parse OpenTherm message from gateway
    unsigned int otMsgType;
    unsigned int otDataId;
    unsigned int otDataValue;
    int mappedItems = sscanf(otgwMessage + 1, "%02x%02x%04x", &otMsgType, &otDataId, &otDataValue); 
    if (mappedItems != 3)
    {
        TRACE("Failed parsing OpenTherm message. Mapped items: %d\n", mappedItems);
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }
    result.msgType = static_cast<OpenThermMsgType>((otMsgType >> 4) & 7);
    result.dataId = static_cast<OpenThermDataId>(otDataId);
    result.dataValue = otDataValue;

    switch (otgwMessage[0])
    {
        case 'T':
            result.direction = OpenThermGatewayDirection::FromThermostat;
            break;

        case 'B':
            result.direction = OpenThermGatewayDirection::FromBoiler;
            break;

        case 'R':
            result.direction = OpenThermGatewayDirection::ToBoiler;
            break;

        case 'A':
            result.direction = OpenThermGatewayDirection::ToThermostat;
            break;

        case 'E':
            result.direction = OpenThermGatewayDirection::Error;
            break;

        default:
            result.direction = OpenThermGatewayDirection::Unexpected;
    }

    TRACE("direction=%d, msgType=%d, dataId=%d, dataValue=0x%04X\n", result.direction, result.msgType, result.dataId, result.dataValue);

    return result;
}


bool OpenThermGateway::sendCommand(const char* cmd, const char* value, char* response)
{
    Tracer tracer("OpenThermGateway::sendCommand", cmd);

    char otgwCommand[16];
    int bufferSize = sizeof(otgwCommand);
    if (snprintf(otgwCommand, bufferSize, "%s=%s\r\n", cmd, value) >= bufferSize)
    {
        TRACE("Command too long");
        return false;
    }

    char* otgwResponse = response ? response : new char[16];
    int retries = 0;
    bool responseReceived;
    do
    {
        // Send OTWG command
        _serial.print(otgwCommand);

        // Read OTWG response
        size_t bytesRead = _serial.readBytesUntil('\n', otgwResponse, sizeof(otgwResponse) - 1);
        otgwResponse[bytesRead] = 0;
        TRACE("OTGW response: %s\n", otgwResponse);
        responseReceived = (strncmp(otgwResponse, cmd, 2) == 0);
    }
    while (!responseReceived && (retries++ < MAX_RETRIES));

    if (!responseReceived)
        TRACE("No valid response from OTGW after %d retries.\n", MAX_RETRIES);

    if (response == NULL)
        delete otgwResponse;

    return responseReceived;
}
