#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#define MAX_GOODWE_INSTANCES 4

class GoodWeInstance;

class GoodWeUDP
{
    public:
        bool begin();
        int discover(int timeoutMs = 1000);
        GoodWeInstance* getInstance(uint8_t instanceId);

        inline const char* getLastError()
        {
            return _lastError;
        }

    private:
        WiFiUDP _udpClient;
        IPAddress _instanceAddresses[MAX_GOODWE_INSTANCES];
        int _instanceCount = 0;
        char _receiveBuffer[256];
        char _lastError[256];

        void setLastError(String format, ...);
        bool sendMessage(const IPAddress& ipAddress, const String& message);
        String receiveMessage(int timeoutMs = 1000);

        friend class GoodWeInstance;
};

class GoodWeInstance
{
    public:
        // Constructor
        GoodWeInstance(const IPAddress& ipAddress, GoodWeUDP& goodWeUDP);

        // Destructor
        ~GoodWeInstance();

        inline const IPAddress& getIPAddress()
        {
            return _ipAddress;
        }

        bool sendATCommand(const String& command);
        bool sendATCommand(const String& command, String& result);

    private:
        const IPAddress& _ipAddress;
        GoodWeUDP& _goodWeUDP;
};

