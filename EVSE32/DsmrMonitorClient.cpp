#include <ArduinoJson.h>
#include <StreamString.h>
#include <Tracer.h>
#include "DsmrMonitorClient.h"

#define MIN_CONTENT_LENGTH 100

StaticJsonDocument<512> _response;

// Constructor
DsmrMonitorClient::DsmrMonitorClient(uint16_t timeout)
{
    isInitialized = false;
    _httpClient.setTimeout(timeout);
     // Re-use TCP connection (using HTTP Keep-Alive)?
     // ESP8266 WebServer Keep-Alive times out after 2 sec, so it's useless to request it.
    _httpClient.setReuse(false);
}

bool DsmrMonitorClient::begin(const char* host)
{
    Tracer tracer(F("DsmrMonitorClient::begin"), host);

    bool result = _httpClient.begin(_wifiClient, host, 80, F("/json"));
    if (!result)
        _lastError = F("Initialization failed");

    isInitialized = true;

    return result;
}

int DsmrMonitorClient::requestData()
{
    Tracer tracer(F("DsmrMonitorClient::requestData"));

    int result = _httpClient.GET();

    if (result < 0)
    {
        _lastError = HTTPClient::errorToString(result);
        return result;
    }
    else if (result != HTTP_CODE_OK)
    {
        _lastError = F("HTTP status code ");
        _lastError += result;
        return result;
    }
    else if (_httpClient.getSize() < MIN_CONTENT_LENGTH)
    {
        _lastError = F("Unexpected Content Length: ");
        _lastError += _httpClient.getSize();
        return 0;
    }

    StreamString jsonResponse;
    jsonResponse.reserve(_httpClient.getSize());

    int bytesRead = _httpClient.writeToStream(&jsonResponse);
    if (bytesRead < 0)
    {
        _lastError = HTTPClient::errorToString(bytesRead);
        return bytesRead;
    }

    return parseJson(jsonResponse) ? HTTP_CODE_OK : 0;
}


bool DsmrMonitorClient::parseJson(String json)
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

    /* TODO
    tIn = _response["Tin"];
    tOut = _response["Tout"];
    tBuffer = _response["Tbuffer"];
    flowRate = _response["Flow"];
    pIn = _response["Pin"];
    valve = _response["Valve"];

    TRACE(F("tIn: %0.1f, tOut: %0.1f, tBuffer: %0.1f\n"), tIn, tOut, tBuffer);
    */

    return true;
}
