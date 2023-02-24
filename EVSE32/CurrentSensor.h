#include <driver/adc.h>
#include <Ticker.h>

class CurrentSensor
{
    public:
        CurrentSensor(uint8_t pin, size_t bufferSize = 1024);

        bool begin(uint16_t zero = 2048, float scale = 0.016F); // Defaults are approximates

        uint16_t calibrateZero();
        float calibrateScale(float actualRMS);

        void measure(uint16_t periods = 5);

        uint16_t inline getSampleCount()
        {
            return _sampleIndex;
        }

        float inline getSample(uint16_t index)
        {
            int intValue = (int)_sampleBufferPtr[index] - _zero;
            return _scale * intValue;
        }

        float getPeak();
        float getRMS();
        float getDC();

        void writeSampleCsv(Print& writeTo, bool raw);

    private:
        uint8_t _pin;
        adc1_channel_t _adcChannel;
        Ticker _ticker;
        uint16_t _sampleBufferSize;
        uint16_t volatile _sampleIndex;
        uint16_t* _sampleBufferPtr = nullptr;
        uint16_t _zero;
        float _scale;

        static void sample(CurrentSensor* instancePtr);
};