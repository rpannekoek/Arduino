#include <OTGW.h>
#include <Tracer.h>

#define OTGW_MESSAGE_SIZE 9
#define MAX_RETRIES 5

OpenThermGatewayMessage OpenThermGateway::readMessage()
{
    Tracer tracer("OpenThermGateway::readMessage");

    OpenThermGatewayMessage result;

    char otgwMessage[OTGW_MESSAGE_SIZE + 1];
    otgwMessage[OTGW_MESSAGE_SIZE] = 0; 
    if (!_serial.readBytes(otgwMessage, OTGW_MESSAGE_SIZE)) 
    {
        result.message = "Timeout";
        result.direction = OpenThermGatewayDirection::Error;
        return result;
    }
    result.message = otgwMessage;

    // Check for gateway errors
    if (strncmp(otgwMessage, "Error", 5) == 0)
    {
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

    int retries = MAX_RETRIES;
    bool responseReceived;
    do
    {
        // Send OTWG command
        _serial.print(otgwMessage);

        // Read OTWG response
        size_t bytesRead = _serial.readBytesUntil('\r', otgwMessage, sizeof(otgwMessage) - 1);
        otgwMessage[bytesRead] = 0;
        TRACE("OpenTherm gateway response: '%s'\n", otgwMessage);
        responseReceived = strncmp(otgwMessage, cmd, 2);
    }
    while (!responseReceived && (retries-- > 0));

    if (retries == 0)
    {
        TRACE("No valid response from OpenTherm gateway after %d retries.", MAX_RETRIES);
        return false;
    }

    // Consume CR/LF
    _serial.readBytes(otgwMessage, 2);

    return true;
}
