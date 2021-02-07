#include "FX.h"

class FXFlanger : public SoundEffect
{
public:
    // Constructor
    FXFlanger(uint16_t sampleRate);

    virtual String getName()
    {
        return F("Flanger");
    }

    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual void filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size);

protected:
    uint16_t _sampleRate;
    uint32_t _delay;
    uint32_t _modulationPeriod;
    uint32_t _modulationDepth;
    int32_t _attenuation;
    float* _sineTable;
};
