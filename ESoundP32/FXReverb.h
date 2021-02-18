#include "FX.h"

class FXReverb : public SoundEffect
{
public:
    virtual String getName()
    {
        return F("Reverb");
    }

    virtual void initialize();
    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual int32_t filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer);

protected:
    uint32_t _delay;
    int32_t _attenuation;
};
