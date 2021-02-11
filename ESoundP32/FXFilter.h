#include "FX.h"
#include "DSP32.h"

class FXFilter : public SoundEffect
{
public:
    virtual String getName()
    {
        return F("Filter");
    }

    virtual void initialize();
    virtual void writeConfigForm(HtmlWriter& html);
    virtual void handleConfigPost(WebServer& webServer);
    virtual int32_t filter(int32_t sample, WaveBuffer& inputBuffer, WaveBuffer& outputBuffer);

private:
    FilterType _filterType;
    float _frequency;
    float _qFactor;
    BiquadCoefficients _coefficients;
    const char* _filterTypeNames[3] = { "LPF", "BPF", "HPF" };
};
