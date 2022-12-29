#include "OTGW.h"
#include <Tracer.h>
#include <PrintFlags.h>
#include <Arduino.h>
#include <Wire.h>

#define BUFFER_SIZE 32
#define WATCHDOG_I2C_ADDRESS 0x26

// For I2C Watchdog protocol/implementation, see:
// https://github.com/rvdbreemen/ESPEasySlaves/blob/master/TinyI2CWatchdog/TinyI2CWatchdog.ino

static const char* _masterStatusNames[5] = {"CH", "DHW", "Cool", "OTC", "CH2"};
static const char* _slaveStatusNames[7] = {"Fault", "CH", "DHW", "Flame", "Cool", "CH2", "Diag"};
static const char* _faultFlagNames[6] = {"Svc", "Lockout", "PWater", "Flame", "PAir", "TWater"};


OpenThermGateway::OpenThermGateway(Stream& serial, uint8_t resetPin, uint32_t readTimeout)
    : _serial(serial), _resetPin(resetPin), _responseTimeoutMs(readTimeout)
{
    memset(errors, 0, sizeof(errors));
    resets = 0;
    Wire.begin();
}


void OpenThermGateway::reset()
{
    Tracer tracer(F("OpenThermGateway::reset"));

    pinMode(_resetPin, OUTPUT);
    digitalWrite(_resetPin, LOW);
    delay(100);
    digitalWrite(_resetPin, HIGH);
    pinMode(_resetPin, INPUT_PULLUP);

    resets++;
}


bool OpenThermGateway::initWatchdog(uint8_t timeoutSeconds)
{
    Tracer tracer(F("OpenThermGateway::initWatchdog"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(6); // SettingsStruct.TimeOut
    Wire.write(timeoutSeconds);
    if (Wire.endTransmission() != 0) return false;

    // Read back SettingsStruct.TimeOut to confirm it's set properly.
    int timeOutReg = readWatchdogData(6);
    return timeOutReg == timeoutSeconds;
}


int OpenThermGateway::readWatchdogData(uint8_t addr)
{
    Tracer tracer(F("OpenThermGateway::readWatchdogData"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(0x83); // Set pointer for byte to read
    Wire.write(addr);
    uint8_t err = Wire.endTransmission();  
    if (err != 0) return -err;
    
    // Request one byte
    if (Wire.requestFrom(WATCHDOG_I2C_ADDRESS, 1) == 0)
        return -10;
    
    return Wire.read(); // Read one byte
}


uint8_t OpenThermGateway::feedWatchdog()
{
    Tracer tracer(F("OpenThermGateway::feedWatchdog"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(0xA5); // Reset watchdog timer
    return Wire.endTransmission();
}


bool OpenThermGateway::readLine()
{
    int i = 0;
    uint32_t millis = 0;
    const uint32_t waitMs = 10;
    const uint32_t timeoutMs = 1000;
    do
    {
        int c = _serial.read();
        if (c < 0)
        {
            delay(waitMs);
            millis += waitMs;
            if (millis > timeoutMs)
            {
                _otgwMessage[i] = 0;
                return false;
            }
        }
        else if (c >= ' ')
            _otgwMessage[i++] = c;
        else if (c == '\n')
            break;
    }
    while (i < sizeof(_otgwMessage) - 1);
    
    _otgwMessage[i] = 0;
    return true;
}


OpenThermGatewayMessage OpenThermGateway::readMessage()
{
    Tracer tracer(F("OpenThermGateway::readMessage"));

    OpenThermGatewayMessage result;

    if (!readLine())
    {
        TRACE(F("Read timeout\n"));
        result.message = _otgwMessage;
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }

    TRACE(F("Message from OTGW: '%s'\n"), _otgwMessage);
    result.message = _otgwMessage;

    // Check for gateway errors
    unsigned int errorCode;
    if (strncmp(_otgwMessage, "Error", 5) == 0)
    {
        if ((sscanf(_otgwMessage + 6, "%x", &errorCode) == 1) && (errorCode > 0) && (errorCode < 5))
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
    int mappedItems = sscanf(_otgwMessage + 1, "%02x%02x%04x", &otMsgType, &otDataId, &otDataValue); 
    if (mappedItems != 3)
    {
        TRACE(F("Failed parsing OpenTherm message. Mapped items: %d\n"), mappedItems);
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }
    result.msgType = static_cast<OpenThermMsgType>((otMsgType >> 4) & 7);
    result.dataId = static_cast<OpenThermDataId>(otDataId);
    result.dataValue = otDataValue;

    switch (_otgwMessage[0])
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

    TRACE(F("direction=%d, msgType=%d, dataId=%d, dataValue=0x%04X\n"), result.direction, result.msgType, result.dataId, result.dataValue);

    return result;
}


bool OpenThermGateway::sendCommand(String cmd, String value)
{
    Tracer tracer(F("OpenThermGateway::sendCommand"), cmd.c_str());

    for (int retries = 0; retries < 2; retries++)
    {
        // Send OTWG command
        _serial.print(cmd);
        _serial.print('=');
        _serial.println(value);

        // Read OTWG response
        // There may be a backlog of non-response messages in the queue, which we will skip here.
        uint32_t timeoutMillis = millis() + _responseTimeoutMs;
        do
        {
            if (readLine())
            {
                if (strncmp(_otgwMessage, cmd.c_str(), 2) == 0)
                {
                    TRACE(F("Response: '%s'\n"), _otgwMessage);
                    return true;
                }
                else
                    TRACE(F("Non-response: '%s'\n"), _otgwMessage);
            }
            else
                TRACE(F("."));
        }
        while (millis() < timeoutMillis);

        // No proper response message is received within the given timeout.
        // Feed the OTGW watchdog and retry sending the command once more.
        TRACE(F("Response timeout\n"));
        feedWatchdog();  
    }

    // No proper response message is received even after a retry.
    return false;
}


bool OpenThermGateway::setResponse(OpenThermDataId dataId, float value)
{
    char valueBuffer[16];

    switch (dataId)
    {
        case OpenThermDataId::MaxTSet:
            snprintf(valueBuffer, sizeof(valueBuffer), "%0.0f", value);
            return sendCommand("SH", valueBuffer); 

        case OpenThermDataId::TOutside:
            snprintf(valueBuffer, sizeof(valueBuffer), "%0.1f", value);
            return sendCommand("OT", valueBuffer); 

        default:
            int16_t dataValue = value * 256.0f;
            snprintf(
                valueBuffer,
                sizeof(valueBuffer),
                "%d:%d,%d",
                dataId,
                dataValue >> 8,
                dataValue & 0xFF);
            return sendCommand("SR", valueBuffer); 
    }
}


const char* OpenThermGateway::getMasterStatus(uint16_t dataValue)
{
    return printFlags((dataValue >> 8), _masterStatusNames, 5, ",");
}


const char* OpenThermGateway::getSlaveStatus(uint16_t dataValue)
{
    return printFlags(dataValue & 0xFF, _slaveStatusNames, 7, ",");
}


const char* OpenThermGateway::getFaultFlags(uint16_t dataValue)
{
    return printFlags((dataValue >> 8), _faultFlagNames, 6, ",");
}


float OpenThermGateway::getDecimal(uint16_t dataValue)
{
    return float(static_cast<int16_t>(dataValue)) / 256;
}
