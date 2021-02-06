#ifndef WAVE_BUFFER_H
#define WAVE_BUFFER_H

#include <Stream.h>
#include "FX.h"

struct WaveStats
{
    int16_t peak;
    float average;
};


class WaveBuffer
{
    public:
        // Constructor
        WaveBuffer(FXEngine& fxEngine) : _fxEngine(fxEngine)
        {
        }

        inline size_t getNumSamples()
        {
            return _numSamples;
        }

        inline size_t getNumNewSamples()
        {
            return _numNewSamples;
        }

        inline size_t getNumClippedSamples()
        {
            return _numClippedSamples;
        }

        inline int getFillPercentage()
        {
            return 100 * _numSamples / _size;
        }

        inline bool isFull()
        {
            return _numSamples == _size;
        }

        inline uint16_t getUpsampleFactor()
        {
            return _upsampleFactor;
        }

        bool begin(size_t size);
        void clear();
        void addSample(int32_t sample);
        size_t getSamples(int16_t* sampleBuffer, size_t numSamples);
        void getNewSamples(int16_t* sampleBuffer, size_t numSamples, size_t minDistance);
        void writeWaveFile(Stream& toStream, uint16_t sampleRate);
        WaveStats getStatistics(size_t frameSize = 0);

    protected:
        FXEngine& _fxEngine;
        size_t _size;
        size_t _numSamples = 0;
        size_t _numNewSamples = 0;
        size_t _numClippedSamples = 0;
        int16_t* _buffer = nullptr;
        uint32_t _index = 0;
        uint16_t _upsampleFactor = 0;
};

#endif