#include "WeatherAPI.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Tracer.h>

#define WEATHER_SERVER_HOST "weerlive.nl"


WeatherAPI::WeatherAPI(int timeout)
    : _timeout(timeout)
{
    _wifiClient.setTimeout(timeout);
}


bool WeatherAPI::beginRequestData(const char* apiKey, const char* location)
{
    Tracer tracer(F("WeatherAPI::beginRequestData"));

    if (!_wifiClient.connect(WEATHER_SERVER_HOST, 80))
    {
        TRACE(F("Unable to connect to host %s\n"), WEATHER_SERVER_HOST);
        return false;
    }

    char* httpRequest = _buffer;
    snprintf(
        httpRequest,
        sizeof(_buffer),
        "GET /api/json-data-10min.php?key=%s&locatie=%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        apiKey,
        location,
        WEATHER_SERVER_HOST
        );
    TRACE(httpRequest);

    size_t httpRequestLength = strlen(httpRequest);
    if (_wifiClient.write(httpRequest, httpRequestLength) != httpRequestLength)
    {
        _wifiClient.stop();
        return false;
    }

    return true;
}


int WeatherAPI::endRequestData()
{
    if (!_wifiClient.available())
        return 0;

    Tracer tracer(F("WeatherAPI::endRequestData"));

    // Read all HTTP headers and obtain HTTP code from first
    int httpCode = 0;
    size_t bytesRead;
    do
    {
        bytesRead = _wifiClient.readBytesUntil('\n', _buffer, sizeof(_buffer) - 1);
        _buffer[bytesRead] = 0;
        TRACE(F("%s\n"), _buffer);

        if (httpCode == 0)
        {
            if ((strlen(_buffer) < 10) || (sscanf(_buffer + 9, "%d", &httpCode) != 1))
                httpCode = WEATHER_ERROR_HTTP_RESPONSE;
            TRACE(F("HTTP code: %d\n"), httpCode);
            if (httpCode != 200)
            {
                close();
                return httpCode;
            }
        }
    } while (bytesRead > 2);
    
    // Read HTTP body and find temperature
    do
    {
        bytesRead = _wifiClient.readBytesUntil(',',  _buffer, sizeof(_buffer) - 1);
        _buffer[bytesRead] = 0;
        TRACE(F("%s\n"), _buffer);

        const char* temp = strstr(_buffer, "\"temp\":");
        if (temp != NULL)
        {
            temp += 9; // "temp": "
            if (sscanf(temp, "%f", &temperature) == 1)
                TRACE(F("Temperature: %0.1f\n"), temperature);
            else
            {
                TRACE(F("Unable to parse temperature\n"));
                httpCode= WEATHER_ERROR_TEMPERATURE;
            }
            break;
        }

    } while (bytesRead > 0);

    if (bytesRead == 0)
    {
        TRACE(F("Temperature not found\n"));
        httpCode = WEATHER_ERROR_NO_TEMPERATURE;
    }

    close();

    return httpCode;
}


int WeatherAPI::requestData(const char* apiKey, const char* location)
{
    Tracer tracer(F("WeatherAPI::requestData"));

    if (!beginRequestData(apiKey, location))
        return WEATHER_ERROR_HTTP_REQUEST;

    int result;
    int waitTime = 0;
    do
    {
        delay(100);
        waitTime += 100;
        result = endRequestData();
    }
    while ((result == 0) && (waitTime < _timeout));
    
    return result;
}


void WeatherAPI::close()
{
    while (_wifiClient.available())
        _wifiClient.read();
    
    _wifiClient.stop();
}