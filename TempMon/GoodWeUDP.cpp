#include "GoodWeUDP.h"
#include <Tracer.h>

#define HF_UDP_PORT 48899
#define HF_DISCOVERY "HF-A11ASSISTHREAD"

bool GoodWeUDP::begin()
{
    Tracer tracer(F("GoodWeUDP::begin"));

    if (!_udpClient.begin(HF_UDP_PORT))
    {
        setLastError(F("Unable to open UDP port %d"), HF_UDP_PORT);
        return false;
    }

    _lastError[0] = 0;
    return true;
}


int GoodWeUDP::discover(int timeoutMs)
{
    Tracer tracer(F("GoodWeUDP::discover"));

    if (!sendMessage(WiFi.broadcastIP(), HF_DISCOVERY))
    {
        setLastError(F("Broadcast failed"));
        return -1;
    }

    // Await discovery response(s)
    _instanceCount = 0;
    int waitMs = 0;
    const int pollIntervalMs = 100;
    do
    {
        String discoveryResponse = receiveMessage(pollIntervalMs);
        if (discoveryResponse.length() > 0)
        {
            int commaIndex = discoveryResponse.indexOf(",");
            if (commaIndex > 0)
            {
                String ipAddress = discoveryResponse.substring(0, commaIndex);
                if (_instanceAddresses[_instanceCount].fromString(ipAddress))
                    _instanceCount++;
            }
        }
        else
            waitMs += pollIntervalMs;
    }
    while ((waitMs < timeoutMs) && (_instanceCount < MAX_GOODWE_INSTANCES));

    TRACE(F("Discovered %d devices.\n"), _instanceCount);
    return _instanceCount;
}


GoodWeInstance* GoodWeUDP::getInstance(uint8_t instanceId)
{
    return (instanceId < _instanceCount) 
        ? new GoodWeInstance(_instanceAddresses[instanceId], *this) 
        : nullptr;  
}


void GoodWeUDP::setLastError(String format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(_lastError, sizeof(_lastError) - 1, format.c_str(), args);
    _lastError[length] = 0;
    va_end(args);

    TRACE("%s\n", _lastError);
}


bool GoodWeUDP::sendMessage(const IPAddress& ipAddress, const String& message)
{
    if (!_udpClient.beginPacket(ipAddress, HF_UDP_PORT))
    {
        TRACE(F("beginPacket failed\n"));
        return false;
    }

    int bytesSent = _udpClient.write(message.c_str(), message.length());

    if (!_udpClient.endPacket())
    {
        TRACE(F("Failed sending %d bytes to %s\n"), bytesSent, ipAddress.toString().c_str());
        return false;
    }

    TRACE(F("Sent '%s' to %s\n"), message.c_str(), ipAddress.toString().c_str());
    return true;
}


String GoodWeUDP::receiveMessage(int timeoutMs)
{
    int waitMs = 0;
    const int delayMs = 10;
    while (_udpClient.parsePacket() == 0)
    {
        delay(delayMs);
        waitMs += delayMs;
        if (waitMs > timeoutMs)
            return String();
   }

    int bytesRead = _udpClient.read(_receiveBuffer, sizeof(_receiveBuffer) - 1);
    _receiveBuffer[bytesRead] = 0;

    TRACE(F("Received '%s' from %s\n"), _receiveBuffer, _udpClient.remoteIP().toString().c_str());
    return _receiveBuffer;
}

// Constructor
GoodWeInstance::GoodWeInstance(const IPAddress& ipAddress, GoodWeUDP& goodWeUDP)
    : _ipAddress(ipAddress), _goodWeUDP(goodWeUDP)
{
    // Start command mode
    _goodWeUDP.sendMessage(_ipAddress, F("+ok"));
}


// Destructor
GoodWeInstance::~GoodWeInstance()
{
    // Stop command mode
    _goodWeUDP.sendMessage(_ipAddress, F("AT+Q\r"));
}


bool GoodWeInstance::sendATCommand(const String& command)
{
    String result;
    return sendATCommand(command, result);
}


bool GoodWeInstance::sendATCommand(const String& command, String& result)
{
    Tracer tracer(F("GoodWeInstance::sendATCommand"), command.c_str());

    String atCommand = "AT+";
    atCommand += command;
    String message = atCommand; 
    message += "\r";
    if (!_goodWeUDP.sendMessage(_ipAddress, message))
    {
        _goodWeUDP.setLastError(F("Failed sending '%s'"), atCommand.c_str());
        return false;
    }

    String response = _goodWeUDP.receiveMessage();
    if (response.length() == 0)
    {
        _goodWeUDP.setLastError(F("No response for '%s'"), atCommand.c_str());
        return false;
    }
    response.trim();

    if (!response.startsWith(F("+ok")))
    {
        _goodWeUDP.setLastError(F("Error for '%s': %s"), atCommand.c_str(), response.c_str());
        return false;
    }

    result = response.substring(4);
    return true;
}
