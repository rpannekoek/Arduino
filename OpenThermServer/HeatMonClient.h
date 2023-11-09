#ifndef HEATMON_CLIENT_H
#define HEATMON_CLIENT_H

#include <ESPHTTPClient.h>

class HeatMonClient
{
    public:
        bool isInitialized;
        float tIn;
        float tOut;
        float tBuffer;
        float flowRate;
        float pIn;
        bool valve;

        // Constructor
        HeatMonClient(uint16_t timeout);

        bool begin(const char* host);
        int requestData();

        inline String getLastError()
        {
            return _lastError;
        }

        inline bool isHeatpumpOn()
        {
            return pIn > 0.5;
        }

    private:
        WiFiClient _wifiClient;
        HTTPClient _httpClient;
        String _lastError;

        bool parseJson(String json);
};


#endif