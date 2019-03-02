#ifndef WIFIFTP_H
#define WIFIFTP_H

#include <c_types.h>
#include <WiFiClient.h>
#include <Print.h>

#define FTP_DEFAULT_CONTROL_PORT 21
#define FTP_DEFAULT_DATA_PORT 22
#define FTP_ERROR_TIMEOUT -1
#define FTP_ERROR_BAD_RESPONSE -2
#define FTP_ERROR_COMMAND_TOO_LONG -3

class WiFiFTPClient
{
    public:
        WiFiFTPClient(int timeout);

        bool begin(const char* host, const char* userName, const char* password, uint16_t port = FTP_DEFAULT_CONTROL_PORT, Print* printTo = nullptr);
        void end();

        int sendCommand(const char* cmd, const char* arg = nullptr, bool awaitResponse = true);
        int readServerResponse(char* responseBuffer = nullptr, size_t responseBufferSize = 0);
        WiFiClient& getDataClient();
        
        WiFiClient& append(const char* filename);

        const char* getLastResponse()
        {
            return _responseBuffer;
        }

    private:
        WiFiClient _controlClient;
        WiFiClient _dataClient;
        char _responseBuffer[128];
        char _cmdBuffer[128];
        int _serverDataPort;
        const char* _host;
        Print* _printPtr;

        bool initialize(const char* userName, const char* password);
};

#endif