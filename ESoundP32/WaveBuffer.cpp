#include <Arduino.h>
#include "WaveBuffer.h"


bool WaveBuffer::begin(size_t size)
{
    _size = size;
    _buffer = (int16_t*) ps_malloc(size * sizeof(int16_t));

    return (_buffer != nullptr);
}


void WaveBuffer::clear()
{
    _index = 0;
    _numSamples = 0;
    memset(_buffer, 0, _size * sizeof(int16_t));
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

    int16_t peak = 0;
    float sum = 0;
    for (int i = 0; i < segment2Size; i++)
    {
        int16_t sample = _buffer[i];
        if (sample < 0) sample = -sample; // abs(sample)
        if (sample > peak) peak = sample;
        sum += sample;
    }
    for (int i = _size - segment1Size; i < _size; i++)
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
