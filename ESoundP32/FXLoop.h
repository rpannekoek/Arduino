#include "FX.h"

class FXLoop : public SoundEffect
{
public:
    virtual String getName()
    {
        return F("Loop");
    }

    virtual void initialize();
    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual int32_t filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer);

protected:
    uint16_t _bpm;
    uint16_t _loopBeats;
    uint16_t _attenuation;
    int32_t _tickAmplitude;
    uint32_t _tickPulseWidth;
    uint32_t _beatLength;
    uint32_t _delay;
    uint32_t _tickIndex;
    uint16_t _tickCount;
};
