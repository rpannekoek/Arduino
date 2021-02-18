#include "FX.h"

class FXModulation : public SoundEffect
{
public:
    virtual String getName()
    {
        return F("Modulation");
    }

    virtual void initialize();
    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual int32_t filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer);

private:
    uint32_t _modulationPeriod;
    uint32_t _modulationIndex;
    float* _sineTable = nullptr;

    void buildSineTable();
};
