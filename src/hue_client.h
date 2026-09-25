#ifndef HUE_CLIENT_H
#define HUE_CLIENT_H

#include "globals.h"

// Active state variables for Philips Hue Bridge connection
extern bool hueLightsEnabled;
extern int hueBrightness;
extern bool hueSunriseEnabled;
extern int hueSunriseDuration;
extern String hueBridgeIP;
extern String hueUsername;

// Temporary edit buffers
extern bool editHueLightsEnabled;
extern int editHueBrightness;
extern bool editHueSunriseEnabled;
extern int editHueSunriseDuration;

// Send direct PUT requests to change local Hue bulb on/off/brightness state
void updateHueLightState();

// Fetch bulb on/off/brightness state from Hue Bridge
void fetchHueLightState();

struct HueScene {
    String id;
    String name;
};
extern HueScene hueScenes[10];
extern int numHueScenes;
extern int selectedHueSceneIndex;
extern String activeHueSceneID;
extern String activeHueSceneName;

// Fetch scenes for HUE_GROUP_ID
void fetchHueScenes();

// Recall a specific scene on HUE_GROUP_ID
void recallHueScene(String sceneId);

// Triggers 2-stage sunrise wakeup transition (brightness 1 instantly, then fade to 254)
void triggerHueSunriseTransition();

// Triggers the sunrise transition asynchronously in a background task
void triggerHueSunriseTransitionAsync();

// Turns off the Hue light instantly (used during early cancel)
void turnOffHueLight();

// Triggers sleep timer fade-out (fades to lavender and 15% brightness over duration)
void startHueSleepTimerFade(int durationMinutes);

#endif // HUE_CLIENT_H
