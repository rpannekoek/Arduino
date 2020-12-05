#include <Stream.h>
#include <String.h>

#define MAX_DATA_LINES 100

class P1Telegram
{
    public:
        enum class PropertyId
        {
            PowerDeliveredTotal = 0,
            PowerReturnedTotal,
            VoltageL1,
            VoltageL2,
            VoltageL3,
            CurrentL1,
            CurrentL2,
            CurrentL3,
            PowerDeliveredL1,
            PowerDeliveredL2,
            PowerDeliveredL3,
            PowerReturnedL1,
            PowerReturnedL2,
            PowerReturnedL3,
            Gas,
            _EndMarker
        };

        String _dataLines[MAX_DATA_LINES];
        size_t _numDataLines;

        // Constructor
        P1Telegram();

        String readFrom(Stream& stream);

        String getPropertyValue(PropertyId propertyId, String* timestampPtr = nullptr);
        float getFloatValue(PropertyId id, String* timestampPtr = nullptr);

    private:
        String _obisIds[static_cast<size_t>(PropertyId::_EndMarker)];
        String _labels[static_cast<size_t>(PropertyId::_EndMarker)];

        void addProperty(PropertyId propertyId, String obisId, String label);
        String readDataLineFrom(Stream& stream);
        void populateTestData(String testId);
};