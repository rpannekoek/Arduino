#ifndef DSP32_H
#define DSP32_H

#include <math.h>

enum struct WindowType
{
    None = 0,
    Hann,
    Blackman,
    BlackmanHarris,
    BlackmanNuttal,
    Nuttal,
    FlatTop
};

struct BinInfo
{
    uint16_t index;
    float minFrequency;
    float maxFrequency;

    float inline getCenterFrequency()
    {
        return (minFrequency + maxFrequency) / 2;
    }
};

struct complex_t
{
    float re;
    float im;

    float inline getPower()
    {
        return sq(re) + sq(im);
    }
    
    float inline getMagnitude()
    {
        return sqrtf(getPower());
    }

    float inline getPhase() // in degrees
    {
        return degrees(atan2f(im, re));
    }
};

class DSP32
{
    public:
        DSP32(bool tracePerformance);

        bool begin(uint16_t frameSize, WindowType windowType, float sampleFrequency = 1);
        void end();

        inline uint16_t getOctaves()
        {
            return _octaves;
        }

        complex_t* runFFT(const int16_t* signal);
        float* getSpectralPower(complex_t* complexSpectrum);
        float* getOctavePower(float* spectralPower);
        BinInfo getFundamental(float* spectralPower);
        BinInfo getBinInfo(uint16_t index);
        BinInfo getOctaveInfo(uint16_t index);
        String getNote(float frequency);

    protected:
        bool _tracePerformance;
        float _sampleFrequency;
        uint16_t _frameSize;
        uint16_t _octaves;
        uint16_t* _octaveStartIndex;
        float* _window;
        complex_t* _fftBuffer;
        float* _fftTableBuffer;
        float* _spectralPower;
        float* _octavePower;
};

#endif