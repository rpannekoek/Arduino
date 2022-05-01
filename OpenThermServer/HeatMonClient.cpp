#include "HeatMonClient.h"
#include <ArduinoJson.h>
#include <Tracer.h>

StaticJsonDocument<256> _response;

// Constructor
HeatMonClient::HeatMonClient(uint16_t timeout)
{
    isInitialized = false;
    _httpClient.setTimeout(timeout);
}

bool HeatMonClient::begin(const char* host)
{
    Tracer tracer(F("HeatMonClient::begin"), host);

    bool result = _httpClient.begin(_wifiClient, host, 80, F("/json"));
    isInitialized = true;
    return result;
}

int HeatMonClient::requestData()
{
    Tracer tracer(F("HeatMonClient::requestData"));

    int result = _httpClient.GET();
    if (result == HTTP_CODE_OK)
    {
        String responseJson = _httpClient.getString();
        TRACE(F("responseJson: '%s'\n"), responseJson.c_str());

        DeserializationError parseError = deserializeJson(_response, responseJson);
        if (parseError == DeserializationError::Ok)
        {
            tIn = _response["Tin"];
            tOut = _response["Tout"];
            tBuffer = _response["Tbuffer"];
            flowRate = _response["Flow"];
            pIn = _response["Pin"];
            valve = _response["Valve"];
    
            TRACE(F("tIn: %0.1f, tOut: %0.1f, tBuffer: %0.1f\n"), tIn, tOut, tBuffer);
        }
        else
        {
            TRACE(F("Failed parsing JSON: %d\n"), parseError);
            result = 0;     
        }
    }

    TRACE(F("result: %d\n"), result);
    return result;
}
