#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#include "globals.h"

struct WeatherSnapshot {
    bool enabled = false;
    bool fetched = false;
    int temp = 0;
    String label;
    int feelsLike = 0;
    float precipitationHours = 0.0f;
    int rainStartHour = -1;
    int precipitationChance = 0;
    int highTemp = 0;
    int uvIndex = 0;
    String uvLabel;
    int codeDaily = 0;
    int tomorrowHighTemp = 0;
    int tomorrowFeelsLike = 0;
    int tomorrowUVIndex = 0;
    String tomorrowUVLabel = "Low";
    float tomorrowPrecipitationHours = 0.0f;
    int tomorrowRainStartHour = -1;
    int tomorrowPrecipChance = 0;
    int tomorrowCodeDaily = 0;
    bool alertActive = false;
    String alertTitle;
    int sunriseHour = 7;
    int sunriseMin = 0;
    int sunsetHour = 20;
    int sunsetMin = 0;
};

void setupWeather();
void updateWeather();
void triggerImmediateWeatherSync();
String getWeatherLabel(int code, bool isDay);
WeatherSnapshot getWeatherSnapshot();

#endif // WEATHER_CLIENT_H
