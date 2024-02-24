#ifndef PERSISTENT_DATA_BASE_H
#define PERSISTENT_DATA_BASE_H

#include <stddef.h>
#include <pgmspace.h>
#include <vector>
#include <HtmlWriter.h>

struct PersistentDataField
{
    PGM_P label;

    virtual void initialize() = 0;
    virtual void validate() = 0;
    virtual void writeHtml(HtmlWriter& html, const String& id) = 0;
    virtual void parse(const String& str) = 0;
};

struct PersistentStringField : PersistentDataField
{
    char* value;
    size_t size;
    PGM_P defaultValue;

    void initialize() override;
    void validate() override;
    void writeHtml(HtmlWriter& html, const String& id) override;
    void parse(const String& str) override;
};

struct PersistentPasswordField : PersistentStringField
{
    void writeHtml(HtmlWriter& html, const String& id) override;
};

struct PersistentIntegerField : PersistentDataField
{
    int* valuePtr;
    int minValue;
    int maxValue;
    int defaultValue;

    void initialize() override;
    void validate() override;
    void writeHtml(HtmlWriter& html, const String& id) override;
    void parse(const String& str) override;
};

struct PersistentTimeSpanField : PersistentIntegerField
{
    void writeHtml(HtmlWriter& html, const String& id) override;
    void parse(const String& str) override;
};

struct PersistentFloatField : PersistentDataField
{
    float* valuePtr;
    int decimals;
    float minValue;
    float maxValue;
    float defaultValue;

    void initialize() override;
    void validate() override;
    void writeHtml(HtmlWriter& html, const String& id) override;
    void parse(const String& str) override;
};

struct PersistentBooleanField : PersistentDataField
{
    bool* valuePtr;
    bool defaultValue;

    void initialize() override;
    void validate() override;
    void writeHtml(HtmlWriter& html, const String& id) override;
    void parse(const String& str) override;
};

struct PersistentDataBase
{
    public:
        PersistentDataBase(size_t dataSize = 0);
        ~PersistentDataBase();

        void begin();
        void writeToEEPROM();
        bool readFromEEPROM();
        void printData();

        virtual void initialize();
        virtual void validate();
        virtual void writeHtmlForm(HtmlWriter& html);
        virtual void parseHtmlFormData(std::function<String(const String&)> formDataById);

    protected:
        void addField(PersistentDataField* fieldPtr, size_t dataSize);
        void addStringField(char* value, size_t size, PGM_P label, PGM_P defaultValue = nullptr);
        void addPasswordField(char* value, size_t size, PGM_P label);
        void addIntegerField(int& value, PGM_P label, int minValue, int maxValue, int defaultValue = 0);
        void addTimeSpanField(int& value, PGM_P label, int minValue, int maxValue, int defaultValue = 0);
        void addFloatField(float& value, PGM_P label, int decimals, float minValue, float maxValue, float defaultValue = 0.0);
        void addBooleanField(bool& value, PGM_P label, bool defaultValue = false, size_t dataSize = sizeof(bool));

    private:
        std::vector<PersistentDataField*> _fields;
        size_t _dataSize;
};

struct BasicWiFiSettings : public PersistentDataBase
{
    char wifiSSID[32];
    char wifiKey[32];
    char hostName[32];
    char ntpServer[32];

    BasicWiFiSettings(PGM_P defaultHostName, size_t additionalDataSize = 0)
        : PersistentDataBase(additionalDataSize)
    {
        addStringField(wifiSSID, sizeof(wifiSSID), PSTR("WiFi SSID"));
        addPasswordField(wifiKey, sizeof(wifiKey), PSTR("WiFi key"));
        addStringField(hostName, sizeof(hostName), PSTR("Host name"), defaultHostName);
        addStringField(ntpServer, sizeof(ntpServer), PSTR("NTP server"), PSTR("europe.pool.ntp.org"));
    }
};

struct WiFiSettingsWithFTP : public BasicWiFiSettings
{
    char ftpServer[32];
    char ftpUser[32];
    char ftpPassword[32];

    WiFiSettingsWithFTP(PGM_P defaultHostName, size_t additionalDataSize = 0)
        : BasicWiFiSettings(defaultHostName, additionalDataSize)
    {
        addStringField(ftpServer, sizeof(ftpServer), PSTR("FTP server"));
        addStringField(ftpUser, sizeof(ftpUser), PSTR("FTP user"));
        addPasswordField(ftpPassword, sizeof(ftpPassword), PSTR("FTP password"));
    }

    inline bool isFTPEnabled()
    {
        return ftpServer[0] != 0;
    }
};

#endif