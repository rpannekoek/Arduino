#include <Arduino.h>
#include <Tracer.h>
#include "FXFilter.h"

#define CFG_TYPE F("Filter_Type")
#define CFG_FREQ F("Filter_Freq")
#define CFG_Q_FACTOR F("Filter_Q")


void FXFilter::initialize()
{
    _filterType = FilterType::BPF;
    _frequency = 1000;
    _qFactor = 2;
    _coefficients = DSP32::calcFilterCoefficients(_filterType, _frequency / _sampleRate, _qFactor);
}


void FXFilter::writeConfigForm(HtmlWriter& html)
{
    html.writeRadioButtons(CFG_TYPE, F("Type"), _filterTypeNames, 3, (int)_filterType);
    html.writeSlider(CFG_FREQ, F("Frequency"), F("Hz"), _frequency, 500, 5000);
    html.writeSlider(CFG_Q_FACTOR, F("Q Factor"), String(), _qFactor * 10, 1, 100, 10);
}


void FXFilter::handleConfigPost(WebServer& webServer)
{
    _filterType = (FilterType) webServer.arg(CFG_TYPE).toInt();
    _frequency = webServer.arg(CFG_FREQ).toFloat();
    _qFactor = webServer.arg(CFG_Q_FACTOR).toFloat() / 10;
    TRACE(
        F("_filterType=%d, _frequency=%0.0f Hz, _qFactor=%0.1f\n"),
        _filterType,
        _frequency,
        _qFactor
        );

    _coefficients = DSP32::calcFilterCoefficients(_filterType, _frequency / _sampleRate, _qFactor);
    TRACE(F("b0=%f\n"), _coefficients.b0);
    TRACE(F("b1=%f\n"), _coefficients.b1);
    TRACE(F("b2=%f\n"), _coefficients.b2);
    TRACE(F("a1=%f\n"), _coefficients.a1);
    TRACE(F("a2=%f\n"), _coefficients.a2);
}


int32_t FXFilter::filter(int32_t sample, ISampleStore& inputBuffer, ISampleStore& outputBuffer)
{
    float output = _coefficients.b0 * sample;
    output += _coefficients.b1 * inputBuffer.getSample(1);
    output += _coefficients.b2 * inputBuffer.getSample(2);
    output -= _coefficients.a1 * outputBuffer.getSample(1);
    output -= _coefficients.a2 * outputBuffer.getSample(2);

    return output;
}
