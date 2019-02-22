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
        TRACE(F("Cannot connect to FTP server '%s' at port %d\n"), host, port);
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
        sendCommand("QUIT");
        _controlClient.stop();
    }

    _printPtr = NULL;
}


bool WiFiFTPClient::initialize(const char* userName, const char* password)
{
    Tracer Tracer(F("WiFiFTPClient::initialize"), userName);

    // Retrieve server welcome message
    int responseCode = readServerResponse();
    bool success = (responseCode >= 200) && (responseCode < 300);
    if (!success)
        return false;

    char cmdBuffer[32];
    int cmdBufferSize = sizeof(cmdBuffer);
    if (snprintf(cmdBuffer, cmdBufferSize, "USER %s", userName) >= cmdBufferSize)
        return false;

    responseCode = sendCommand(cmdBuffer);
    if (responseCode == 331)
    {
        // User name OK, password required.
        if (snprintf(cmdBuffer, cmdBufferSize, "PASS %s", password) >= cmdBufferSize)
            return false;
        responseCode = sendCommand(cmdBuffer);
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
        const char* token = strtok(NULL, ",)");
        if (token == NULL)
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


int WiFiFTPClient::sendCommand(const char* cmd, bool awaitResponse)
{
    Tracer Tracer(F("WiFiFTPClient::sendCommand"), cmd);

    if (_printPtr != NULL)
        _printPtr->println(cmd);

    _controlClient.println(cmd);

    if (!awaitResponse)
        return 0;

    return readServerResponse();   
}


int WiFiFTPClient::readServerResponse()
{
    Tracer tracer(F("WiFiFTPClient::readServerResponse"));

    size_t bytesRead = _controlClient.readBytesUntil('\n', _responseBuffer, sizeof(_responseBuffer) - 1);
    _responseBuffer[bytesRead] = 0;
    TRACE(F("Response: %s\n"), _responseBuffer);

    if (_printPtr != NULL)
        _printPtr->print(_responseBuffer);

    if (bytesRead < 3)
        return FTP_ERROR_TIMEOUT;

    int responseCode;
    if (sscanf(_responseBuffer, "%d", &responseCode) != 1)
        return FTP_ERROR_BAD_RESPONSE;

    TRACE(F("Response code: %d\n"), responseCode);
    return responseCode;
}


WiFiClient& WiFiFTPClient::getDataClient()
{
    Tracer tracer(F("WiFiFTPClient::getDataClient"));

    if (!_dataClient.connect(_host, _serverDataPort))
        TRACE(F("Unable to connect to server data port %d\n"), _serverDataPort);

    return _dataClient;
}


WiFiClient& WiFiFTPClient::append(const char* filename)
{
    Tracer tracer(F("WiFiFTPClient::append"), filename);

    snprintf(_cmdBuffer, sizeof(_cmdBuffer), "APPE %s", filename);
    sendCommand(_cmdBuffer, false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() != 150)
            dataClient.stop();
    }

    return dataClient;
}
