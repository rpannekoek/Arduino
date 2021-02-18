#include "FX.h"

class FXFlanger : public SoundEffect
{
public:
    virtual String getName()
    {
        return F("Flanger");
    }

    virtual void initialize();
    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual int32_t filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer);

protected:
    uint32_t _delay;
    uint32_t _modulationPeriod;
    uint32_t _modulationDepth;
    uint32_t _modulationIndex;
    int32_t _attenuation;
    float* _sineTable;
};
