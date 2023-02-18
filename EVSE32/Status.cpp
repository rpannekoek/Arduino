#include <Tracer.h>
#include "Status.h"

#define BLACK pixelFromRGB(0, 0, 0)
#define BLUE pixelFromRGB(0, 0, 255)
#define GREEN pixelFromRGB(0, 255, 0)
#define GREEN_BREATHE pixelFromRGBW(0, 255, 0, 1)
#define CYAN pixelFromRGB(0, 255, 255)
#define RED pixelFromRGB(255, 0, 0)
#define MAGENTA pixelFromRGB(255, 0, 255)
#define YELLOW pixelFromRGB(255, 255, 0)
#define YELLOW_BREATHE pixelFromRGBW(255, 255, 0, 1)
#define WHITE pixelFromRGB(255, 255, 255)

#define BREATHE_INTERVAL 0.1F
#define BREATHE_STEPS 48


const char* EVSEStateNames[] = 
{
    [EVSEState::Booting] = "Booting",
    [EVSEState::SelfTest] = "Self Test",
    [EVSEState::Failure] = "Failure",
    [EVSEState::Ready] = "Ready",
    [EVSEState::Authorize] = "Authorize",
    [EVSEState::AwaitCharging] = "Await charging",
    [EVSEState::Charging] = "Charging",
    [EVSEState::ChargeCompleted] = "Charge completed"
};

pixelColor_t StatusLED::_statusColors[] =
{
    [EVSEState::Booting] = BLUE,
    [EVSEState::SelfTest] = MAGENTA,
    [EVSEState::Failure] = RED,
    [EVSEState::Ready] = GREEN_BREATHE,
    [EVSEState::Authorize] = WHITE,
    [EVSEState::AwaitCharging] = CYAN,
    [EVSEState::Charging] = YELLOW_BREATHE,
    [EVSEState::ChargeCompleted] = BLACK
};

float StatusLED::_breatheTable[] =
{
    1.000,
    0.996,
    0.983,
    0.962,
    0.933,
    0.897,
    0.854,
    0.804,
    0.750,
    0.691,
    0.629,
    0.565,
    0.500,
    0.435,
    0.371,
    0.309,
    0.250,
    0.196,
    0.146,
    0.103,
    0.067,
    0.038,
    0.017,
    0.004,
    0.000
};


StatusLED::StatusLED(int8_t pin)
{
    _ledStrand.rmtChannel = 0;
    _ledStrand.gpioNum = pin;
    _ledStrand.ledType = LED_SK6812_V1;
    _ledStrand.brightLimit = 255;
    _ledStrand.numPixels = 1;
}


bool StatusLED::begin()
{
    Tracer tracer(F("StatusLED::begin"));

    pinMode(_ledStrand.gpioNum, OUTPUT);

    int rc = digitalLeds_initDriver();
    if (rc != ESP_OK)
    {
        TRACE(F("digitalLeds_initDriver returned %d\n"), rc);
        return false;
    }

    strand_t* ledStrands[] = { &_ledStrand };
    rc = digitalLeds_addStrands(ledStrands, 1);
    if (rc != ESP_OK)
    {
        TRACE(F("digitalLeds_addStrands returned %d\n"), rc);
        return false;
    }
   
    return setStatus(EVSEState::Booting);
}


bool StatusLED::setColor(pixelColor_t color)
{
    //TRACE(F("StatusLED::setColor(%d, %d, %d)\n"), color.r, color.g, color.b);

    pixelColor_t& pixel = _ledStrand.pixels[0]; 
    pixel.r = color.g; // NOTE: Red & Green are swapped
    pixel.g = color.r;
    pixel.b = color.b;

    strand_t* ledStrands[] = { &_ledStrand };
    int rc = digitalLeds_drawPixels(ledStrands, 1);
    if (rc != 0)
    {
        TRACE(F("digitalLeds_drawPixels returned %d\n"), rc);
        return false;
    }

    return true;
}


bool StatusLED::setStatus(EVSEState status)
{
    Tracer tracer(F("StatusLED::setStatus"), EVSEStateNames[status]);

    _statusColor = _statusColors[status];

    if (_statusColor.w != 0)
    {
        _breatheIndex = 0;
        _breatheTicker.attach(BREATHE_INTERVAL, breathe, this);
    }
    else
        _breatheTicker.detach();

    return setColor(_statusColor);
}


void StatusLED::breathe(StatusLED* instancePtr)
{
    instancePtr->breathe();
}


void StatusLED::breathe()
{
    int i = (_breatheIndex <= BREATHE_STEPS / 2) ? _breatheIndex : BREATHE_STEPS - _breatheIndex;
    float f = _breatheTable[i];
    pixelColor_t breatheColor = pixelFromRGB(f * _statusColor.r, f * _statusColor.g, f * _statusColor.b);

    setColor(breatheColor);
    
    if (++_breatheIndex == BREATHE_STEPS)
        _breatheIndex = 0;
}