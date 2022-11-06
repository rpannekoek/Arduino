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
    if (!result)
        _lastError = F("Initialization failed");

    isInitialized = true;
    return result;
}

int HeatMonClient::requestData()
{
    Tracer tracer(F("HeatMonClient::requestData"));

    int result = _httpClient.GET();

    switch (result)
    {
        case HTTP_CODE_OK:
            if (!parseJson( _httpClient.getString()))
                result = 0;
            break;

        case HTTPC_ERROR_CONNECTION_FAILED:
            _lastError = F("Connection failed");
            break;

        case HTTPC_ERROR_SEND_HEADER_FAILED:
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
            _lastError = F("Send failed");
            break;

        case HTTPC_ERROR_READ_TIMEOUT:
            _lastError = F("Read timeout");
            break;

        default:
            if (result > 0)
                _lastError = F("HTTP status code ");
            else
                _lastError = F("Error ");
            _lastError += result;
            break;
    }

    TRACE(F("result: %d\n"), result);
    return result;
}


bool HeatMonClient::parseJson(String json)
{
    TRACE(F("JSON: '%s'\n"), json.c_str());

    DeserializationError parseError = deserializeJson(_response, json);
    if (parseError != DeserializationError::Ok)
    {
        _lastError = F("JSON error: "); 
        _lastError += parseError.c_str();
        TRACE(_lastError);
        return false;     
    }

    tIn = _response["Tin"];
    tOut = _response["Tout"];
    tBuffer = _response["Tbuffer"];
    flowRate = _response["Flow"];
    pIn = _response["Pin"];
    valve = _response["Valve"];

    TRACE(F("tIn: %0.1f, tOut: %0.1f, tBuffer: %0.1f\n"), tIn, tOut, tBuffer);
    return true;
}
