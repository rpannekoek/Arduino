#include <Arduino.h>
#include <Tracer.h>
#include "WaveBuffer.h"


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


bool WaveBuffer::begin(size_t size)
{
    Tracer tracer(F("WaveBuffer::begin"));

    _size = size;
    _buffer = (int16_t*) ps_malloc(size * sizeof(int16_t));

    return (_buffer != nullptr);
}


void WaveBuffer::clear()
{
    Tracer tracer(F("WaveBuffer::clear"));

    _index = 0;
    _numSamples = 0;
    _numNewSamples = 0;
    _numClippedSamples = 0;
    memset(_buffer, 0, _size * sizeof(int16_t));
}


void WaveBuffer::addSample(int32_t sample)
{
    _fxEngine.filter(sample, _buffer, _index, _size);

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

    if (_index == _size) _index = 0;
    _buffer[_index++] = sample;

    if (_numSamples < _size) _numSamples++;
    if (_numNewSamples < _size) _numNewSamples++;
}


size_t WaveBuffer::getSamples(int16_t* sampleBuffer, size_t numSamples)
{
    if (numSamples > _numSamples) numSamples = _numSamples;
    size_t segment2Size = (numSamples <= _index) ? numSamples : _index;
    size_t segment1Size = numSamples - segment2Size;
    if (segment1Size > 0)
        memcpy(sampleBuffer, _buffer + _size - segment1Size, segment1Size * sizeof(int16_t));
    if (segment2Size > 0)
        memcpy(sampleBuffer + segment1Size, _buffer + _index - segment2Size, segment2Size * sizeof(int16_t));
    return numSamples;
}


void WaveBuffer::getNewSamples(int16_t* sampleBuffer, size_t numSamples, size_t minDistance)
{
    if (numSamples + minDistance > _numNewSamples)
    {
        // Reached the minimum distance: return zeroes
        memset(sampleBuffer, 0, numSamples * sizeof(int16_t));
        return;
    }

    // Slipping buffer implementation
    _upsampleFactor = 0;
    int distanceFactor = (_numNewSamples - minDistance) / numSamples;
    if (distanceFactor < 8)
    {
        _upsampleFactor = 1 << distanceFactor;
        numSamples -= numSamples / _upsampleFactor;
    }

    int32_t index = _index - _numNewSamples;
    if (index < 0) index += _size;

    int j = 0;
    int16_t previousSample = 0;
    for (int i = 0; i < numSamples; i++)
    {
        int16_t sample = _buffer[index];
        if ((_upsampleFactor != 0) && ((i % _upsampleFactor) == 1)) 
        {
            // Add an interpolated sample
            int32_t interpolatedSample = sample;
            interpolatedSample += previousSample;
            interpolatedSample /= 2;
            sampleBuffer[j++] = interpolatedSample;
        }
        sampleBuffer[j++] = sample;

        previousSample = sample;
        if (++index == _size) index = 0;
    }

    _numNewSamples -= numSamples;
}


void WaveBuffer::writeWaveFile(Stream& toStream, uint16_t sampleRate)
{
    const uint16_t bytesPerSample = sizeof(int16_t);
    uint32_t dataSize = _numSamples * bytesPerSample; 
    uint32_t fileSize = sizeof(WaveHeader) + dataSize;
    WaveHeader header =
    {
        .chunkID = { 'R', 'I', 'F', 'F' },
        .chunkSize = fileSize - 8,
        .format = { 'W', 'A', 'V', 'E' },

        .subChunk1ID = { 'f', 'm', 't', ' '},
        .subChunk1Size = 16,
        .audioFormat = 1, // LPCM
        .numChannels = 1,
        .sampleRate = sampleRate,
        .byteRate = static_cast<uint32_t>(sampleRate * bytesPerSample),
        .blockAlign = bytesPerSample,
        .bitsPerSample = bytesPerSample * 8,

        .subChunk2ID = { 'd', 'a', 't', 'a' },
        .subChunk2Size = dataSize
    };
    toStream.write((const uint8_t*)&header, sizeof(WaveHeader));

    size_t segment2Size = (_numSamples <= _index) ? _numSamples : _index;
    size_t segment1Size = _numSamples - segment2Size;
    if (segment1Size > 0)
        toStream.write((const uint8_t*)(_buffer + _size - segment1Size), segment1Size * sizeof(int16_t));   
    if (segment2Size > 0)
        toStream.write((const uint8_t*)(_buffer + _index - segment2Size), segment2Size * sizeof(int16_t));        
}


WaveStats WaveBuffer::getStatistics(size_t frameSize)
{
    if (frameSize == 0 || frameSize > _numSamples) frameSize = _numSamples;
    size_t segment2Size = (frameSize <= _index) ? frameSize : _index;
    size_t segment1Size = frameSize - segment2Size;
    uint32_t segment1Start = _size - segment1Size; 
    uint32_t segment2Start = _index - segment2Size; 

    int16_t peak = 0;
    float sum = 0;
    for (uint32_t i = segment2Start; i < _index; i++)
    {
        int16_t sample = _buffer[i];
        if (sample < 0) sample = -sample; // abs(sample)
        if (sample > peak) peak = sample;
        sum += sample;
    }
    for (uint32_t i = segment1Start; i < _size; i++)
    {
        int16_t sample = _buffer[i];
        if (sample < 0) sample = -sample; // abs(sample)
        if (sample > peak) peak = sample;
        sum += sample;
    }

    WaveStats result = 
    { 
        .peak = peak,
        .average = sum / frameSize
    };
    return result;
}
