#ifndef WAVE_BUFFER_H
#define WAVE_BUFFER_H

#include <Stream.h>

struct WaveHeader
{
    char chunkID[4];
    uint32_t chunkSize;
    char format[4];

    char subChunk1ID[4];
    uint32_t subChunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;

    char subChunk2ID[4];
    uint32_t subChunk2Size;
} __attribute__((packed));


struct WaveStats
{
    int16_t peak;
    float average;
};

class WaveBuffer
{
    public:
        inline size_t getNumSamples()
        {
            return _numSamples;
        }

        inline int getFillPercentage()
        {
            return 100 * _numSamples / _size;
        }

        inline bool isFull()
        {
            return _numSamples == _size;
        }

        inline void addSample(int16_t sample)
        {
            if (_index == _size)
                _index = 0;

            if (_numSamples < _size)
                _numSamples++;

            _buffer[_index++] = sample;
        }

        bool begin(size_t size);
        void clear();
        size_t getSamples(int16_t* sampleBuffer, size_t numSamples);
        void writeWaveFile(Stream& toStream, uint16_t sampleRate);
        WaveStats getStatistics(size_t frameSize = 0);

    protected:
        size_t _size;
        size_t _numSamples = 0;
        int16_t* _buffer = nullptr;
        uint32_t volatile _index = 0;
};

#endif