#include <RESTClient.h>
#include <ArduinoJson.h>
#include <Tracer.h>

// Constructor
RESTClient::RESTClient(uint16_t timeout, JsonDocument* responseDocPtr)
    : _responseDoc(*responseDocPtr)
{
    _timeout = timeout;
    isInitialized = false;
}


bool RESTClient::begin(const String& baseUrl)
{
    _baseUrl = baseUrl;
    _requestPending = false;
    isInitialized = true;
    return isInitialized;
}


int RESTClient::requestData(const String& urlSuffix)
{
    if (!_requestPending)
    {
        String url = _baseUrl + urlSuffix;
        TRACE(F("HTTP GET %s\n"), url.c_str());
        if (!_asyncHttpRequest.open("GET", url.c_str()))
        {
            _lastError = F("Open failed");
            return HTTP_OPEN_FAILED;
        }
        if (!_asyncHttpRequest.send())
        {
            _lastError = F("Send failed");
            return HTTP_SEND_FAILED;
        }
        _asyncHttpRequest.setTimeout(_timeout);
        _requestPending = true;
        return HTTP_REQUEST_PENDING;
    }

    if (_asyncHttpRequest.readyState() != readyStateDone)
    {
        return HTTP_REQUEST_PENDING;
    }

    _requestPending = false;
    int httpCode = _asyncHttpRequest.responseHTTPcode();

    if (httpCode < 0)
    {
        _lastError = _asyncHttpRequest.responseHTTPString();
        return httpCode;
    }

    if (httpCode != HTTP_OK)
    {
        _lastError = F("HTTP ");
        _lastError += httpCode;
        return httpCode;
    }

    String responseBody = _asyncHttpRequest.responseText();
    TRACE(F("HTTP response after %d ms: %s\n"), _asyncHttpRequest.elapsedTime(), responseBody.c_str());

    DeserializationError parseError = parseJson(responseBody);
    if (parseError != DeserializationError::Ok)
    {
        _lastError = F("JSON error: "); 
        _lastError += parseError.c_str();
        return RESPONSE_PARSING_FAILED;
    }

    return parseResponse(_responseDoc)
        ? HTTP_OK
        : RESPONSE_PARSING_FAILED;
}


DeserializationError RESTClient::parseJson(const String& json)
{
    return deserializeJson(_responseDoc, json);
}
