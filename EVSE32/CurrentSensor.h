#include <Ticker.h>

class CurrentSensor
{
    public:
        CurrentSensor(int8_t pin);

        bool begin(uint16_t zero = 2048, float scale = 0.016F); // Defaults are approximates

        uint16_t calibrateZero();
        float calibrateScale(float actualRMS);

        uint16_t measure(uint16_t periods = 1);

        float inline getSample(uint16_t index)
        {
            int intValue = (int)_sampleBufferPtr[index] - _zero;
            return _scale * intValue;
        }

        float getPeak();
        float getRMS();
        float getDC();

    private:
        int8_t _pin;
        Ticker _ticker;
        uint16_t _sampleBufferSize;
        uint16_t volatile _sampleIndex;
        uint16_t* _sampleBufferPtr = nullptr;
        uint16_t _zero;
        float _scale;

        static void sample(CurrentSensor* instancePtr);
};