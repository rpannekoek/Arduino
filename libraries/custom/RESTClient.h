#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include <AsyncHTTPRequest_Generic.hpp>
#include <ArduinoJson.h>

#define HTTP_REQUEST_PENDING 0
#define HTTP_OK 200
#define HTTP_OPEN_FAILED -100
#define HTTP_SEND_FAILED -101
#define RESPONSE_PARSING_FAILED -102

class RESTClient
{
    public:
        bool isInitialized;

        // Constructor
        RESTClient(uint16_t timeout, JsonDocument* responseDocPtr);

        int requestData(const String& urlSuffix = "");

        inline String getLastError()
        {
            return _lastError;
        }

        inline bool isRequestPending()
        {
            return _requestPending;
        }

    protected:
        AsyncHTTPRequest _asyncHttpRequest;
        JsonDocument& _responseDoc;
        String _baseUrl;
        String _lastError;
        uint16_t _timeout;
        bool _requestPending;

        bool begin(const String& baseUrl);
        virtual DeserializationError parseJson(const String& json);
        virtual bool parseResponse(const JsonDocument& response) = 0;
};

#endif