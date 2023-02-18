#ifndef HEATMON_CLIENT_H
#define HEATMON_CLIENT_H

#include <HTTPClient.h>

struct PhaseData
{
    String Name;
    float U;
    float I;
    float Pdelivered;
    float Preturned;

    PhaseData()
    {
    }

    PhaseData(const PhaseData& other)
    {
        Name = other.Name;
        U = other.U;
        I = other.I;
        Pdelivered = other.Pdelivered;
        Preturned = other.Preturned;
    }
};

class DsmrMonitorClient
{
    public:
        bool isInitialized;

        // Constructor
        DsmrMonitorClient(uint16_t timeout);

        bool begin(const char* host);
        int requestData();

        String inline getLastError()
        {
            return _lastError;
        }

        std::vector<PhaseData> inline getElectricity()
        {
            return _electricity;
        }

    private:
        WiFiClient _wifiClient;
        HTTPClient _httpClient;
        String _lastError;
        std::vector<PhaseData> _electricity;

        bool parseJson(String json);
};


#endif