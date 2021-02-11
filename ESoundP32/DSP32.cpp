#define CONFIG_DSP_OPTIMIZED true

#include <esp_dsp.h>
#include <Tracer.h>
#include "DSP32.h"

// Constructor
DSP32::DSP32(bool tracePerformance)
    : _tracePerformance(tracePerformance)
{
}


bool DSP32::begin(uint16_t frameSize, WindowType windowType, float sampleFrequency)
{
    Tracer tracer(F("DSP32::begin"));

    _fftTableBuffer = (float*) ps_malloc(frameSize * sizeof(float));
    if (_fftTableBuffer == nullptr)
    {
        TRACE(F("Allocating FFT table buffer failed\n"));
        return false;
    }

    esp_err_t dsps_result = dsps_fft2r_init_fc32(_fftTableBuffer, frameSize);
    if (dsps_result  != ESP_OK)
    {
        TRACE(F("dsps_fft2r_init_fc32() returned %X\n"), dsps_result);
        return false;
    }

    _sampleFrequency = sampleFrequency;
    _frameSize = frameSize;
    _octaves = log2l(frameSize) - 1;

    _fftBuffer = (complex_t*) ps_malloc(frameSize * sizeof(complex_t));
    _spectralPower = (float*) ps_malloc((frameSize/2 + 1) * sizeof(complex_t));
    _octavePower = new float[_octaves];
    _octaveStartIndex = new uint16_t[_octaves];

    uint16_t octaveStartIndex = 1;
    uint16_t octaveWidth = 1;
    for (int i = 0; i < _octaves; i++)
    {
        _octaveStartIndex[i] = octaveStartIndex;
        octaveStartIndex += octaveWidth;
        octaveWidth *= 2;
    }

    _window = (float*) ps_malloc(frameSize * sizeof(float));
    switch (windowType)
    {
        case WindowType::None:
            for (int i = 0; i < frameSize; i++)
                _window[i] = 1.0;
            break;
            
        case WindowType::Hann:
            dsps_wind_hann_f32(_window, frameSize);
            break;

        case WindowType::Blackman:
            dsps_wind_blackman_f32(_window, frameSize);
            break;

        case WindowType::BlackmanHarris:
            dsps_wind_blackman_harris_f32(_window, frameSize);
            break;

        case WindowType::BlackmanNuttal:
            dsps_wind_blackman_nuttall_f32(_window, frameSize);
            break;

        case WindowType::Nuttal:
            dsps_wind_nuttall_f32(_window, frameSize);
            break;

        case WindowType::FlatTop:
            dsps_wind_flat_top_f32(_window, frameSize);
            break;
        
        default:
            TRACE(F("Unexpected window type: %i\n"), windowType);
            return false;
    }

    // Scale window so full scale => 1.0
    for (int i = 0; i < frameSize; i++)
        _window[i] /= 32768;

    return true;
}


void DSP32::end()
{
    Tracer tracer(F("DSP32::end"));

    free(_fftTableBuffer);
    free(_fftBuffer);
    free(_spectralPower);
    delete[] _octavePower;
    delete[] _octaveStartIndex;
    free(_window);

    _fftBuffer = nullptr;
    _spectralPower = nullptr;
    _octavePower = nullptr;
    _octaveStartIndex = nullptr;
    _window = nullptr;

    dsps_fft2r_deinit_fc32();
}


complex_t* DSP32::runFFT(const int16_t* signal)
{
    // Load real integer signal into complex float array
    // Apply Window function and rescale to 1.0 full scale
    uint32_t loadStartCycles = xthal_get_ccount();
    for (int i = 0; i < _frameSize; i++)
    {
        _fftBuffer[i].re = float(signal[i]) * _window[i];
        _fftBuffer[i].im = 0;
    }
    uint32_t loadEndCycles = xthal_get_ccount();

    // FFT core
    uint32_t fftStartCycles = xthal_get_ccount();
    dsps_fft2r_fc32_ae32((float*)_fftBuffer, _frameSize);
    uint32_t fftEndCycles = xthal_get_ccount();

    // Bit reverse FFT output
    uint32_t bitrevStartCycles = xthal_get_ccount();
    dsps_bit_rev_fc32((float*)_fftBuffer, _frameSize);
    uint32_t bitrevEndCycles = xthal_get_ccount();

    if (_tracePerformance)
    {
        TRACE(F("Loading %i samples into windowed complex array took %u cycles\n"), _frameSize, loadEndCycles - loadStartCycles);
        TRACE(F("FFT core took %u cycles\n"), fftEndCycles - fftStartCycles);
        TRACE(F("Bit reversal took %u cycles\n"), bitrevEndCycles - bitrevStartCycles);
    }

    return _fftBuffer;
}


float* DSP32::getSpectralPower(complex_t* complexSpectrum)
{
    uint32_t startCycles = xthal_get_ccount();
    for (int i = 0; i <= _frameSize / 2; i++)
    {
        _spectralPower[i] = complexSpectrum[i].getPower();
    }
    uint32_t endCycles = xthal_get_ccount();

    if (_tracePerformance)
    {
        TRACE(F("Getting spectral power took %u cycles\n"), endCycles - startCycles);
    }

    return _spectralPower;
}


float* DSP32::getOctavePower(float* spectralPower)
{
    uint32_t startCycles = xthal_get_ccount();
    memset(_octavePower, 0, sizeof(float) * _octaves);

    // Group spectral power in octave bins
    int octave = 0;
    int octaveLength = 1;
    int nextOctaveIndex = 2;
    for (int i = 1; i < _frameSize / 2; i++)
    {
        if (i == nextOctaveIndex)
        {
            octave++;
            octaveLength *= 2;
            nextOctaveIndex += octaveLength;
        }
        _octavePower[octave] += spectralPower[i]; 
    }
    uint32_t endCycles = xthal_get_ccount();

    // Normalize to 0 dBFS
    float scale = 4.0 / float(sq(_frameSize));
    for (int i = 0; i <= octave; i++)
    {
        _octavePower[i] *= scale;
    }

    if (_tracePerformance)
    {
        TRACE(F("Getting octave power took %u cycles\n"), endCycles - startCycles);
    }

    return _octavePower;
}


BinInfo DSP32::getFundamental(float* spectralPower)
{
    uint32_t startCycles = xthal_get_ccount();
    // Find largest spectral peak using Harmonic Product Spectrum technique
    const float floor = 1e-4;
    float maxHPS = 0;
    int peakIndex = 0;
    for (int i = 1; i <= _frameSize / 6; i++)
    {
        float sp2 = spectralPower[i * 2] + floor;
        float sp3 = spectralPower[i * 3] + floor;
        float hps = spectralPower[i] * sp2 * sp3;
        //TRACE(F("HPS[%i] = %g\n"), i, hps);
        if (hps > maxHPS)
        {
            maxHPS = hps;
            peakIndex = i;
        }
    }
    uint32_t endCycles = xthal_get_ccount();

    if (_tracePerformance)
    {
        TRACE(F("HPS calculation took %u cycles.\n"), endCycles - startCycles);
    }

    return getBinInfo(peakIndex);
}


BinInfo DSP32::getBinInfo(uint16_t index)
{
    float binWidth = _sampleFrequency / _frameSize;
    float minFreq = (index == 0) ? 0 : binWidth * (float(index) - 0.5);
    float maxFreq = (index == 0 || index == _frameSize / 2) ?
         minFreq + binWidth / 2 : 
         minFreq + binWidth;

    BinInfo result
    {
        .index = index,
        .minFrequency = minFreq,
        .maxFrequency = maxFreq
    };
    return result;
} 


BinInfo DSP32::getOctaveInfo(uint16_t index)
{
    float minFreq = getBinInfo(_octaveStartIndex[index]).minFrequency;
    float maxFreq = (index == _octaves - 1) ? 
        _sampleFrequency / 2 : 
        getBinInfo(_octaveStartIndex[index + 1]).minFrequency;

    BinInfo result
    {
        .index = index,
        .minFrequency = minFreq,
        .maxFrequency = maxFreq
    };
    return result;
}


String DSP32::getNote(float frequency)
{
    static const char* notes[] = { "A", "Bb", "B", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab" };
    const float a0Frequency = 27.5;

    int noteIndex = roundf(log2f(frequency / a0Frequency) * 12);
    if (noteIndex < 0) noteIndex = 0;
    int octave = noteIndex / 12;

    String result = String(notes[noteIndex % 12]);
    result += octave;
    return result;
}


BiquadCoefficients DSP32::calcFilterCoefficients(FilterType filterType, float f, float qFactor)
{
    BiquadCoefficients result;
    float* coefficients = (float*) &result;
    esp_err_t err;
    switch (filterType)
    {
        case FilterType::LPF:
            err = dsps_biquad_gen_lpf_f32(coefficients, f, qFactor);
            break;

        case FilterType::BPF:
            err = dsps_biquad_gen_bpf_f32(coefficients, f, qFactor);
            break;

        case FilterType::HPF:
            err = dsps_biquad_gen_hpf_f32(coefficients, f, qFactor);
            break;

        default:
            TRACE(F("Unexpected filter type.\n"));
    }

    if (err != ESP_OK)
    {
        TRACE(F("Error calculating coefficients: %X\n"), err);
    }

    return result;
}
