#ifndef WMATA_CLIENT_H
#define WMATA_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

enum WmataMode {
    WMATA_MODE_OFF = 0,
    WMATA_MODE_MORNINGS = 1,
    WMATA_MODE_ALL_DAY = 2
};

// Morning mode fetch/display window (24-hour local time)
#define WMATA_MORNING_START_HOUR 5
#define WMATA_MORNING_END_HOUR 9

bool isWmataVisible();

extern String wmataApiKey;
extern String wmataStopId;
extern String wmataRouteFilter;
extern WmataMode wmataMode;

extern String wmataLine1Text;
extern String wmataLine2Text;
extern bool wmataDataValid;

void setupWmata();
void updateWmataBusPredictions();
void invalidateWmataCache();
String getWmataLine1();
String getWmataLine2();
String truncateStopName(const String& rawStopName);

#endif // WMATA_CLIENT_H
