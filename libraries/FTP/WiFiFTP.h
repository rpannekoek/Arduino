#ifndef WIFIFTP_H
#define WIFIFTP_H

#include <c_types.h>
#include <WiFiClient.h>

#define FTP_DEFAULT_CONTROL_PORT 21
#define FTP_DEFAULT_DATA_PORT 22
#define FTP_ERROR_TIMEOUT -1
#define FTP_ERROR_BAD_RESPONSE -2

class WiFiFTPClient
{
    public:
        WiFiFTPClient(int timeout);

        bool begin(const char* host, const char* userName, const char* password, uint16_t port = FTP_DEFAULT_CONTROL_PORT);
        void end();

        int sendCommand(const char* cmd, bool awaitResponse = true);
        int readServerResponse();
        WiFiClient& getDataClient();
        
        const char* getLastResponse()
        {
            return _responseBuffer;
        }

    private:
        WiFiClient _controlClient;
        WiFiClient _dataClient;
        char _responseBuffer[64];
        int _serverDataPort;
        const char* _host;

        bool initialize(const char* userName, const char* password);
};

#endif