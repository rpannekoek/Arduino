#ifndef WIFIFTP_H
#define WIFIFTP_H

#include <stdint.h>
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

        bool passive();
        int sendCommand(String cmd, const char* arg = nullptr, bool awaitResponse = true);
        int readServerResponse(char* responseBuffer = nullptr, size_t responseBufferSize = 0);
        WiFiClient& getDataClient();

        WiFiClient& store(const char* filename);
        WiFiClient& append(const char* filename);

        inline const char* getLastError()
        {
            return _lastError;
        }

        void setUnexpectedResponse(const char* response = nullptr);

    private:
        WiFiClient _controlClient;
        WiFiClient _dataClient;
        String _lastCommand;
        char _responseBuffer[128];
        int _serverDataPort;
        const char* _host;
        Print* _printPtr;
        char _lastError[128];

        bool initialize(const char* userName, const char* password);
        void setLastError(String format, ...);
};

#endif