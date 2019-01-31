#include <OTGW.h>
#include <Tracer.h>
#include <Arduino.h>

#define OTGW_MESSAGE_SIZE 11
#define MAX_RETRIES 5
#define OTGW_RESET_PIN 14

OpenThermGateway::OpenThermGateway(Stream& serial)
    : _serial(serial)
{
    memset(errors, 0, sizeof(errors));
}


void OpenThermGateway::reset()
{
    Tracer tracer("OpenThermGateway::reset");

    digitalWrite(OTGW_RESET_PIN, LOW);
    delay(1);
    digitalWrite(OTGW_RESET_PIN, HIGH);
}


OpenThermGatewayMessage OpenThermGateway::readMessage()
{
    Tracer tracer("OpenThermGateway::readMessage");

    OpenThermGatewayMessage result;

    char otgwMessage[OTGW_MESSAGE_SIZE + 1];
    size_t bytesRead = _serial.readBytesUntil('\n',  otgwMessage, OTGW_MESSAGE_SIZE);
    if (bytesRead == 0) 
    {
        result.message = "Timeout";
        result.direction = OpenThermGatewayDirection::Error;
        return result;
    }
    otgwMessage[bytesRead] = 0;
    result.message = otgwMessage;

    // Check for gateway errors
    unsigned int errorCode;
    if (sscanf(otgwMessage + 1, "Error %02X", &errorCode) == 1)
    {
        if ((errorCode > 0) && (errorCode < 5))
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
    if (sscanf(otgwMessage + 1, "%02X%02X%04X", &otMsgType, &otDataId, &otDataValue) != 3)
    {
        result.direction = OpenThermGatewayDirection::Error;
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

        default:
            result.direction = OpenThermGatewayDirection::Error;
    }

    TRACE("direction=%d, msgType=%d, dataId=%d, dataValue=0x%04X", result.direction, result.msgType, result.dataid, result.dataValue);

    return result;
}


bool OpenThermGateway::sendCommand(const char* cmd, const char* value)
{
    Tracer tracer("OpenThermGateway::sendCommand", cmd);

    char otgwMessage[16];
    if (snprintf(otgwMessage, sizeof(otgwMessage), "%s=%s\r\n", cmd, value) >= sizeof(otgwMessage))
    {
        TRACE("Command too long");
        return false;
    }

    int retries = 0;
    bool responseReceived;
    do
    {
        // Send OTWG command
        _serial.print(otgwMessage);

        // Read OTWG response
        size_t bytesRead = _serial.readBytesUntil('\n', otgwMessage, sizeof(otgwMessage) - 1);
        otgwMessage[bytesRead] = 0;
        TRACE("OpenTherm gateway response: %s", otgwMessage);
        responseReceived = strncmp(otgwMessage, cmd, 2);
    }
    while (!responseReceived && (retries++ < MAX_RETRIES));

    if (!responseReceived)
        TRACE("No valid response from OpenTherm gateway after %d retries.", MAX_RETRIES);

    return responseReceived;
}
