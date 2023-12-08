#include <RESTClient.h>

class OTGWClient : public RESTClient
{
    public:
        String boilerLevel;

        // Constructor
        OTGWClient(uint16_t timeout = 10);

        bool begin(const char* host);
        int setPump(bool on, const String& reason = "");
        int retry();

    protected:
        String _urlSuffix;

        virtual bool parseResponse(const JsonDocument& response);
};
