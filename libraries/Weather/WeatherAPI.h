#ifndef WEATHER_API_H
#define WEATHER_API_H

#define WEATHER_ERROR_URL -100
#define WEATHER_ERROR_TEMPERATURE -101
#define WEATHER_ERROR_NO_TEMPERATURE -102


class WeatherAPI
{
    public:
        float temperature;

        WeatherAPI(int timeout);

        int requestData(const char* apiKey, const char* location);
    
    protected:
        int _timeout;
};

#endif