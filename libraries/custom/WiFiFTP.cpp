#include "WiFiFTP.h"
#include <Tracer.h>


WiFiFTPClient::WiFiFTPClient(int timeout)
{
    _controlClient.setTimeout(timeout);
    _dataClient.setTimeout(timeout);

    _responseBuffer[0] = 0;
    _lastError[0] = 0;
}


bool WiFiFTPClient::begin(const char* host, const char* userName, const char* password, uint16_t port, Print* printTo)
{
    Tracer Tracer(F("WiFiFTPClient::begin"), host);

    _printPtr = printTo;

    if (!_controlClient.connect(host, port))
    {
        setLastError(F("Cannot connect to %s:%d"), host, port);
        return false;
    }
    _host = host;

    bool success = initialize(userName, password);
    if (!success)
    {
        TRACE(F("Unable to initialize FTP server\n"));
        end();
    }

    return success;
}


void WiFiFTPClient::end()
{
    Tracer Tracer(F("WiFiFTPClient::end"));

    if (_dataClient.connected())
        _dataClient.stop();

    if (_controlClient.connected())
    {
        sendCommand(F("QUIT"), nullptr, false);
        // We want to read (and print) the FTP server response for QUIT,
        // but we don't want to return it in getLastResponse(), so we use a separate response buffer.
        char responseBuffer[16];
        readServerResponse(responseBuffer, sizeof(responseBuffer));
        _controlClient.stop();
    }

    _printPtr = nullptr;
}


void WiFiFTPClient::setLastError(String format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(_lastError, sizeof(_lastError) - 1, format.c_str(), args);
    _lastError[length] = 0;
    va_end(args);

    TRACE("ERROR: %s\n", _lastError);
}


void WiFiFTPClient::setUnexpectedResponse(const char* response)
{
    if (response == nullptr)
        response = _responseBuffer;

    if (response[0] == 0)
        setLastError(F("No response for '%s'"), _lastCommand.c_str());
    else
        setLastError(F("Unexpected for '%s': %s"), _lastCommand.c_str(), response);
}


bool WiFiFTPClient::initialize(const char* userName, const char* password)
{
    Tracer Tracer(F("WiFiFTPClient::initialize"), userName);

    // Retrieve server welcome message
    _lastCommand = F("[Connect]");
    int responseCode = readServerResponse();
    bool success = (responseCode >= 200) && (responseCode < 300);
    if (!success)
    {
        setUnexpectedResponse();
        return false;
    }

    responseCode = sendCommand(F("USER"), userName);
    if (responseCode == 331)
    {
        // User name OK, password required.
        responseCode = sendCommand(F("PASS"), password);
    }

    if (responseCode != 230)
    {
        setUnexpectedResponse();
        return false;
    }

    responseCode = sendCommand(F("PASV"));
    if (responseCode != 227)
    {
        setUnexpectedResponse();
        return false;
    }
    
    // Parse server data port
    int params[6];
    strtok(_responseBuffer, "(");
    for (int i = 0; i < 6; i++) 
    {
        const char* token = strtok(nullptr, ",)");
        if (token == nullptr)
        {
            setLastError(F("Unable to parse PASV response"));
            return false;
        }
        params[i] = atoi(token);
    }   
    _serverDataPort = (params[4] << 8) + params[5];
    TRACE(F("Server data port: %d\n"), _serverDataPort);

    return true;
}


int WiFiFTPClient::sendCommand(String cmd, const char* arg, bool awaitResponse)
{
    Tracer Tracer(F("WiFiFTPClient::sendCommand"), cmd.c_str());

    _lastCommand = cmd;
    if (arg != nullptr)
    {
        _lastCommand += " ";
        _lastCommand += arg;
    }

    if (_printPtr != nullptr)
        _printPtr->println(_lastCommand);

    _controlClient.println(_lastCommand);

    return awaitResponse ? readServerResponse() : 0;
}


int WiFiFTPClient::readServerResponse(char* responseBuffer, size_t responseBufferSize)
{
    Tracer tracer(F("WiFiFTPClient::readServerResponse"));

    if (responseBuffer == nullptr)
    {
        responseBuffer = _responseBuffer;
        responseBufferSize = sizeof(_responseBuffer);
    }

    size_t bytesRead = _controlClient.readBytesUntil('\n', responseBuffer, responseBufferSize - 1);
    responseBuffer[bytesRead] = 0;
    TRACE(F("Response: '%s'\n"), responseBuffer);

    if (_printPtr != nullptr)
        _printPtr->print(responseBuffer);

    if (bytesRead == 0)
        return FTP_ERROR_TIMEOUT;

    int responseCode;
    if (sscanf(responseBuffer, "%d", &responseCode) != 1)
        return FTP_ERROR_BAD_RESPONSE;

    TRACE(F("Response code: %d\n"), responseCode);
    return responseCode;
}


WiFiClient& WiFiFTPClient::getDataClient()
{
    Tracer tracer(F("WiFiFTPClient::getDataClient"));

    if (!_dataClient.connect(_host, _serverDataPort))
    {
        setLastError(F("Cannot connect to %s:%d"), _host, _serverDataPort);
    }

    return _dataClient;
}


WiFiClient& WiFiFTPClient::store(const char* filename)
{
    Tracer tracer(F("WiFiFTPClient::store"), filename);

    sendCommand(F("STOR"), filename, false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() != 150)
        {
            setUnexpectedResponse();
            dataClient.stop();
        }
    }

    return dataClient;
}


WiFiClient& WiFiFTPClient::append(const char* filename)
{
    Tracer tracer(F("WiFiFTPClient::append"), filename);

    sendCommand(F("APPE"), filename, false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() != 150)
        {
            setUnexpectedResponse();
            dataClient.stop();
        }
    }

    return dataClient;
}
