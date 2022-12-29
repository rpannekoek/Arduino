#include "WiFiFTP.h"
#include <Tracer.h>


WiFiFTPClient::WiFiFTPClient(int timeout)
{
    _controlClient.setTimeout(timeout);
    _dataClient.setTimeout(timeout);

    _responseBuffer[0] = 0;
}


bool WiFiFTPClient::begin(const char* host, const char* userName, const char* password, uint16_t port, Print* printTo)
{
    Tracer Tracer(F("WiFiFTPClient::begin"), host);

    _printPtr = printTo;

    if (!_controlClient.connect(host, port))
    {
        snprintf(_responseBuffer, sizeof(_responseBuffer), "Cannot connect to '%s' port %u", host, port);
        TRACE(F("%s\n"), _responseBuffer);
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

    // TODO: read remaining data from control/data clients?

    if (_dataClient.connected())
        _dataClient.stop();

    if (_controlClient.connected())
    {
        sendCommand("QUIT", nullptr, false);
        // We want to read (and print) the FTP server response for QUIT,
        // but we don't want to return it in getLastResponse(), so we abuse the command buffer.
        readServerResponse(_cmdBuffer, sizeof(_cmdBuffer));
        _controlClient.stop();
    }

    _printPtr = nullptr;
}


bool WiFiFTPClient::initialize(const char* userName, const char* password)
{
    Tracer Tracer(F("WiFiFTPClient::initialize"), userName);

    // Retrieve server welcome message
    int responseCode = readServerResponse();
    bool success = (responseCode >= 200) && (responseCode < 300);
    if (!success)
        return false;

    responseCode = sendCommand("USER", userName);
    if (responseCode == 331)
    {
        // User name OK, password required.
        responseCode = sendCommand("PASS", password);
    }

    if (responseCode != 230)
        return false;

    responseCode = sendCommand("PASV");
    if (responseCode != 227)
        return false;
    
    // Parse server data port
    int params[6];
    strtok(_responseBuffer, "(");
    for (int i = 0; i < 6; i++) 
    {
        const char* token = strtok(nullptr, ",)");
        if (token == nullptr)
        {
            TRACE(F("Unable to parse PASV response\n"));
            return false;
        }
        params[i] = atoi(token);
    }   
    _serverDataPort = (params[4] << 8) + params[5];
    TRACE(F("Server data port: %d\n"), _serverDataPort);

    return true;
}


int WiFiFTPClient::sendCommand(const char* cmd, const char* arg, bool awaitResponse)
{
    Tracer Tracer(F("WiFiFTPClient::sendCommand"), cmd);

    int cmdBufferSize = sizeof(_cmdBuffer);
    int cmdLength;
    if (arg == nullptr)
    {
        strncpy(_cmdBuffer, cmd, cmdBufferSize);
        cmdLength = strlen(cmd);
    }
    else
        cmdLength = snprintf(_cmdBuffer, cmdBufferSize, "%s %s", cmd, arg);
    if (cmdLength >= cmdBufferSize)
        return FTP_ERROR_COMMAND_TOO_LONG;

    if (_printPtr != nullptr)
        _printPtr->println(_cmdBuffer);

    _controlClient.println(_cmdBuffer);

    if (awaitResponse)
        return readServerResponse();
    else
        return 0;
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
    TRACE(F("Response: %s\n"), responseBuffer);

    if (_printPtr != nullptr)
        _printPtr->print(responseBuffer);

    if (bytesRead < 3)
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
        snprintf(_responseBuffer, sizeof(_responseBuffer), "Cannot connect to data port %d", _serverDataPort);
        TRACE(F("%s\n"), _responseBuffer);
    }

    return _dataClient;
}


WiFiClient& WiFiFTPClient::store(const char* filename)
{
    Tracer tracer(F("WiFiFTPClient::store"), filename);

    sendCommand("STOR", filename, false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() != 150)
            dataClient.stop();
    }

    return dataClient;
}


WiFiClient& WiFiFTPClient::append(const char* filename)
{
    Tracer tracer(F("WiFiFTPClient::append"), filename);

    sendCommand("APPE", filename, false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() != 150)
            dataClient.stop();
    }

    return dataClient;
}
