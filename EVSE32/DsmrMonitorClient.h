#ifndef HEATMON_CLIENT_H
#define HEATMON_CLIENT_H

#include <HTTPClient.h>

struct PhaseData
{
    const char* Phase;
    float U;
    float I;
    float Pdelivered;
    float Preturned;
};

class DsmrMonitorClient
{
    public:
        bool isInitialized;
        PhaseData phases[4];

        // Constructor
        DsmrMonitorClient(uint16_t timeout);

        bool begin(const char* host);
        int requestData();

        inline String getLastError()
        {
            return _lastError;
        }

    private:
        WiFiClient _wifiClient;
        HTTPClient _httpClient;
        String _lastError;

        bool parseJson(String json);
};


#endif