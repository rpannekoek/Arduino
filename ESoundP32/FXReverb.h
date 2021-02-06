#include "FX.h"

class FXReverb : public SoundEffect
{
public:
    // Constructor
    FXReverb(uint16_t sampleRate);

    virtual String getName()
    {
        return F("Reverb");
    }

    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual void filter(int32_t& newSample, const int16_t* buffer, uint32_t index, size_t size);

protected:
    uint16_t _sampleRate;
    uint32_t _delay;
    int32_t _attenuation;
};
