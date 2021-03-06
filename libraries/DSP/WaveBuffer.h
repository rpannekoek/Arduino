#ifndef WAVE_BUFFER_H
#define WAVE_BUFFER_H

#include <Stream.h>

struct WaveStats
{
    int16_t peak;
    float average;
};


class ISampleStore
{
    public:
        virtual int16_t getSample(uint32_t delay) = 0;
};

class ISampleBuffer : public ISampleStore
{
    public:
        virtual void addSample(int32_t sample) = 0;
        virtual void addSamples(int32_t* samples, uint32_t numSamples) = 0;
};


class WaveBuffer : public ISampleBuffer
{
    public:
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

        inline int16_t getNewSample()
        {
            if (_numNewSamples == 0) return 0;
            int32_t index = _index - _numNewSamples--;
            if (index < 0) index += _size;
            return _buffer[index];
        }

        bool begin(size_t size);
        void clear();
        virtual void addSample(int32_t sample);
        virtual void addSamples(int32_t* samples, uint32_t numSamples);
        virtual int16_t getSample(uint32_t delay);
        size_t getSamples(int16_t* sampleBuffer, size_t numSamples);
        void getNewSamples(int16_t* sampleBuffer, size_t numSamples);
        void writeWaveFile(Stream& toStream, uint16_t sampleRate);
        WaveStats getStatistics(size_t frameSize = 0);

    private:
        size_t _size;
        size_t _numSamples = 0;
        size_t _numNewSamples = 0;
        size_t _numClippedSamples = 0;
        int16_t* _buffer = nullptr;
        uint32_t _index = 0;

        inline int16_t clipSample(int32_t sample)
        {
            // Ensure the sample fits in 16 bits
            if (sample > 32767) 
            {
                sample = 32767;
                _numClippedSamples++;
            }
            if (sample < -32768)
            {
                sample = -32768;
                _numClippedSamples++;
            }
            return sample;
        }
};

#endif