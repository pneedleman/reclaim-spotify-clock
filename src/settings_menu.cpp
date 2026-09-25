#include "settings_menu.h"
#include "hue_client.h"
#include "spotify_client.h"
#include "display_manager.h"
#include "audio_controller.h"
#include "timer_manager.h"
#include <freertos/semphr.h>
#include <freertos/task.h>

// Define Preferences object
Preferences preferences;
static SemaphoreHandle_t nvsMutex = NULL;

// Helper: ensure NVS mutex is created and take it if the scheduler is running
static inline void nvsLock() {
    if (nvsMutex == NULL) nvsMutex = xSemaphoreCreateMutex();
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTake(nvsMutex, portMAX_DELAY);
    }
}
static inline void nvsUnlock() {
    if (nvsMutex != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreGive(nvsMutex);
    }
}

// Define Alarm & settings variables (persisted to NVS)
int alarmHour = DEFAULT_ALARM_HOUR;
int alarmMinute = DEFAULT_ALARM_MINUTE;
bool alarmEnabled = DEFAULT_ALARM_ENABLED;
int alarmVolume = 12;
int alarmSoundIndex = 0;
bool backlightState = true;
bool gentleWakeEnabled = true;
bool autoDimmingEnabled = true;
int nightBigClock = 0;
int nightBrightness = 2;
int dayBrightness = 100;
AlarmRepeatMode alarmRepeatMode = REPEAT_SCHOOL_SCHEDULE;
AlarmRepeatMode editAlarmRepeatMode = REPEAT_SCHOOL_SCHEDULE;
const char* REPEAT_MODES[] = {"Once", "Everyday", "Weekdays", "Weekends", "School"};

// Define Non-blocking timers & display states
unsigned long lastLCDUpdate = 0;
unsigned long volumeDisplayTimeout = 0;
unsigned long pendingSaveSettingsTime = 0;
unsigned long menuInactivityTimeout = 0;
bool enteredSettingsFromMusicMode = false;
extern int lastCreatedCharScreenIndex;

// Define Menu State
MenuState currentMenuState = MENU_NAVIGATING;
MenuLevel currentMenuLevel = LEVEL_MAIN;
int currentMenuItem = 0;

// Morning affirmations (shown when alarm is dismissed)
String affirmations[15];

// Default affirmation list
static const char* defaultAffirmations[] = {
    "You got this! \x07",
    "Sparkle today! \x07",
    "Aesthetic queen\x07",
    "Have a magic day",
    "Go shine! \x07",
    "Today is yours!\x07",
    "Smile! \x07",
    "Make it count! \x07",
    "Be kind today \x07",
    "Dream big! \x07",
    "Stay golden \x07",
    "Beautiful mind \x07",
    "Live Laugh Love \x07",
    "Keep on dancing\x07",
    "Create magic! \x07"
};

// Define Temporary submenu variables being edited
int editSleepMinutes = 0;
int editTimerMinutes = 15;
bool editBacklight = true;
int editNightBrightness = 2;
int editDayBrightness = 100;
bool editAlarmEnabled = true;
int editAlarmVol = 12;
int editAlarmSound = 0;
bool editGentleWake = true;
bool editWeatherEnabled = true;
bool editSchoolCalendar = true;
int editNightBigClock = 2;
bool editAutoDimming = true;
int editAlarmHour = 7;
int editAlarmMinute = 0;
int editHueSceneIndex = 0;
WmataMode editWmataMode = WMATA_MODE_OFF;
int editDeviceVolume = 10;

// Define Active sleep timer trackers
unsigned long sleepTimerEnd = 0;
unsigned long sleepDurationMinutes = 0;

// Define Active snooze variables
int snoozeHour = 0;
int snoozeMinute = 0;
bool snoozeActive = false;
int snoozeDurationSelection = 0;
int lastAlarmDismissedDay = -1;

// Define Gentle wake ramp trackers
unsigned long gentleWakeStartTime = 0;
bool gentleWakeActive = false;

// Define Scheduled Reminder variables
bool reminderEnabled = false;
int reminderHour = 7;
int reminderMinute = 30;
String reminderText = "";
String reminderSound = "silent";

// Special Calendar Countdown
CountdownEvent countdownEvents[MAX_COUNTDOWN_EVENTS];

// Static Data-Driven Menu Descriptor Tables
static const MenuItemDescriptor MAIN_MENU[] = {
    {"1. Alarm >",            MENU_TYPE_SUBMENU, LEVEL_ALARM_CONFIG, nullptr, 0, 0, 0, nullptr, 0},
    {"2. Spotify >",          MENU_TYPE_SUBMENU, LEVEL_SPOTIFY_SETTINGS, nullptr, 0, 0, 0, nullptr, 0},
    {"3. Audio >",            MENU_TYPE_SUBMENU, LEVEL_AUDIO_SETTINGS, nullptr, 0, 0, 0, nullptr, 0},
    {"4. Hue Lights >",       MENU_TYPE_SUBMENU, LEVEL_HUE_LIGHTS, nullptr, 0, 0, 0, nullptr, 0},
    {"5. Display >",          MENU_TYPE_SUBMENU, LEVEL_DISPLAY_CONFIG, nullptr, 0, 0, 0, nullptr, 0},
    {"6. Device Info >",      MENU_TYPE_SUBMENU, LEVEL_DEVICE_INFO, nullptr, 0, 0, 0, nullptr, 0},
    {"7. Exit Menu",          MENU_TYPE_ACTION,  0, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor ALARM_CONFIG_MENU[] = {
    {"1. Alarm State",        MENU_TYPE_TOGGLE, 0, &editAlarmEnabled, 0, 1, 1, nullptr, 0},
    {"2. Alarm Hour",         MENU_TYPE_NUMBER, 0, &editAlarmHour, 0, 23, 1, nullptr, 0},
    {"3. Alarm Min",          MENU_TYPE_NUMBER, 0, &editAlarmMinute, 0, 59, 1, nullptr, 0},
    {"4. Repeat Mode",        MENU_TYPE_SELECT, 0, &editAlarmRepeatMode, 0, 4, 1, REPEAT_MODES, 5},
    {"5. Alarm Sound",        MENU_TYPE_NUMBER, 0, &editAlarmSound, 0, NUM_ALARM_SOUNDS - 1, 1, nullptr, 0},
    {"6. Alarm Vol",          MENU_TYPE_NUMBER, 0, &editAlarmVol, 0, 21, 1, nullptr, 0},
    {"7. Gentle Wake",        MENU_TYPE_TOGGLE, 0, &editGentleWake, 0, 1, 1, nullptr, 0},
    {"8. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor AUDIO_SETTINGS_MENU[] = {
    {"1. Speaker Vol",        MENU_TYPE_NUMBER, 0, &editDeviceVolume, 0, 21, 1, nullptr, 0},
    {"2. Bedside Timer",      MENU_TYPE_NUMBER, 0, &editTimerMinutes, 0, 60, 5, nullptr, 0},
    {"3. Sleep Timer",        MENU_TYPE_NUMBER, 0, &editSleepMinutes, 0, 45, 5, nullptr, 0},
    {"4. White Noise",        MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"5. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor DEVICE_INFO_MENU[] = {
    {"Spotify Clock",         MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"Firmware: v1.0.5",      MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"Storage: SPIFFS",       MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"Author: Antigrav",      MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"5. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor SPOTIFY_SETTINGS_MENU[] = {
    {"1. Sel. Device",        MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"2. Sel Playlst",        MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"3. Shuffle",            MENU_TYPE_TOGGLE, 0, &editSpotifyShuffleState, 0, 1, 1, nullptr, 0},
    {"4. Exit Music",         MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"5. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor HUE_LIGHTS_MENU[] = {
    {"1. Lights",             MENU_TYPE_TOGGLE, 0, &editHueLightsEnabled, 0, 1, 1, nullptr, 0},
    {"2. Brightness",         MENU_TYPE_NUMBER, 0, &editHueBrightness, 0, 100, 5, nullptr, 0},
    {"3. Hue Scenes",         MENU_TYPE_ACTION, 0, nullptr, 0, 0, 0, nullptr, 0},
    {"4. Sunrise Wake",       MENU_TYPE_TOGGLE, 0, &editHueSunriseEnabled, 0, 1, 1, nullptr, 0},
    {"5. Sunrise Dur",        MENU_TYPE_NUMBER, 0, &editHueSunriseDuration, 10, 30, 10, nullptr, 0},
    {"6. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const char* WMATA_MODES[] = {"Off", "Mornings", "All Day"};

static const char* NIGHT_BIG_CLOCK_MODES[] = {"Off", "On", "Night"};

static const MenuItemDescriptor DISPLAY_CONFIG_MENU[] = {
    {"1. LCD Backlight",     MENU_TYPE_TOGGLE, 0, &editBacklight, 0, 1, 1, nullptr, 0},
    {"2. Auto Dimming",      MENU_TYPE_TOGGLE, 0, &editAutoDimming, 0, 1, 1, nullptr, 0},
    {"3. Big Clock",          MENU_TYPE_SELECT, 0, &editNightBigClock, 0, 2, 1, NIGHT_BIG_CLOCK_MODES, 3},
    {"4. Show Weather",       MENU_TYPE_TOGGLE, 0, &editWeatherEnabled, 0, 1, 1, nullptr, 0},
    {"5. Show Calendar",      MENU_TYPE_TOGGLE, 0, &editSchoolCalendar, 0, 1, 1, nullptr, 0},
    {"6. Show WMATA",         MENU_TYPE_SELECT, 0, &editWmataMode, 0, 2, 1, WMATA_MODES, 3},
    {"7. < Back",             MENU_TYPE_BACK,   LEVEL_MAIN, nullptr, 0, 0, 0, nullptr, 0}
};

static const MenuItemDescriptor* getMenuDescriptor(MenuLevel level, int& size) {
    switch (level) {
        case LEVEL_MAIN:
            size = sizeof(MAIN_MENU) / sizeof(MenuItemDescriptor);
            return MAIN_MENU;
        case LEVEL_ALARM_CONFIG:
            size = sizeof(ALARM_CONFIG_MENU) / sizeof(MenuItemDescriptor);
            return ALARM_CONFIG_MENU;
        case LEVEL_SPOTIFY_SETTINGS:
            size = sizeof(SPOTIFY_SETTINGS_MENU) / sizeof(MenuItemDescriptor);
            return SPOTIFY_SETTINGS_MENU;
        case LEVEL_HUE_LIGHTS:
            size = sizeof(HUE_LIGHTS_MENU) / sizeof(MenuItemDescriptor);
            return HUE_LIGHTS_MENU;
        case LEVEL_DISPLAY_CONFIG:
            size = sizeof(DISPLAY_CONFIG_MENU) / sizeof(MenuItemDescriptor);
            return DISPLAY_CONFIG_MENU;
        case LEVEL_AUDIO_SETTINGS:
            size = sizeof(AUDIO_SETTINGS_MENU) / sizeof(MenuItemDescriptor);
            return AUDIO_SETTINGS_MENU;
        case LEVEL_DEVICE_INFO:
            size = sizeof(DEVICE_INFO_MENU) / sizeof(MenuItemDescriptor);
            return DEVICE_INFO_MENU;
    }
    size = 0;
    return nullptr;
}

// NVS Settings Persistence
void loadSettings() {
    nvsLock();
    preferences.begin("alarm_clock", true); // read-only
    alarmHour = preferences.getInt("hour", DEFAULT_ALARM_HOUR);
    alarmMinute = preferences.getInt("minute", DEFAULT_ALARM_MINUTE);
    alarmEnabled = preferences.getBool("enabled", DEFAULT_ALARM_ENABLED);
    currentVolume = preferences.getInt("volume", 10);
    alarmVolume = preferences.getInt("alm_vol", 12);
    alarmSoundIndex = preferences.getInt("alm_snd", 0);
    backlightState = preferences.getBool("bklight", true);
    gentleWakeEnabled = preferences.getBool("gentle", true);
    alarmRepeatMode = (AlarmRepeatMode)preferences.getInt("alm_rpt", (int)REPEAT_SCHOOL_SCHEDULE);
    
    // hueLightsEnabled and hueBrightness are polled from the Hue bridge, not stored in NVS
    hueSunriseEnabled = preferences.getBool("hue_wake", true);
    hueSunriseDuration = preferences.getInt("hue_dur", 30);
    hueBridgeIP = preferences.getString("hue_ip", HUE_BRIDGE_IP);
    hueUsername = preferences.getString("hue_user", HUE_BRIDGE_USERNAME);
    activeHueSceneID = preferences.getString("hue_scid", "");
    activeHueSceneName = preferences.getString("hue_scnm", "None");
    weatherEnabled = preferences.getBool("weather", true);
    schoolCalendarEnabled = preferences.getBool("sch_cal_en", true);
    autoDimmingEnabled = preferences.getBool("auto_dim", true);
    int nbc = preferences.getInt("night_big", -1);
    if (nbc < 0 || nbc > 2) {
        nbc = preferences.getBool("night_big", true) ? 2 : 0;
    }
    nightBigClock = nbc;
    nightBrightness = preferences.getInt("night_bri", 2);
    dayBrightness = preferences.getInt("day_bri", 100);
    reminderEnabled = preferences.getBool("rem_on", false);
    reminderHour = preferences.getInt("rem_hr", 7);
    reminderMinute = preferences.getInt("rem_min", 30);
    reminderText = preferences.getString("rem_txt", "");
    reminderSound = preferences.getString("rem_snd", "silent");
    lastAlarmDismissedDay = preferences.getInt("last_dism_day", -1);
    
    for (int i = 0; i < MAX_COUNTDOWN_EVENTS; i++) {
        char enKey[12]; snprintf(enKey, sizeof(enKey), "cnt_%d_en", i);
        char nameKey[16]; snprintf(nameKey, sizeof(nameKey), "cnt_%d_name", i);
        char yKey[10]; snprintf(yKey, sizeof(yKey), "cnt_%d_y", i);
        char mKey[10]; snprintf(mKey, sizeof(mKey), "cnt_%d_m", i);
        char dKey[10]; snprintf(dKey, sizeof(dKey), "cnt_%d_d", i);
        countdownEvents[i].enabled = preferences.getBool(enKey, false);
        countdownEvents[i].name = preferences.getString(nameKey, "");
        countdownEvents[i].year = preferences.getInt(yKey, 0);
        countdownEvents[i].month = preferences.getInt(mKey, 0);
        countdownEvents[i].day = preferences.getInt(dKey, 0);
    }
    
    wmataApiKey = preferences.getString("wmata_key", WMATA_DEFAULT_API_KEY);
    wmataStopId = preferences.getString("wmata_stop", WMATA_DEFAULT_STOP_ID);
    wmataRouteFilter = preferences.getString("wmata_route", WMATA_DEFAULT_ROUTE);
    wmataMode = (WmataMode)preferences.getInt("wmata_mode", (int)WMATA_MODE_OFF);

    for (int i = 0; i < 15; i++) {
        String key = "affirm" + String(i);
        affirmations[i] = preferences.getString(key.c_str(), defaultAffirmations[i]);
    }

    preferences.end();
    
    // Bounds check volume and options
    if (currentVolume < 0) currentVolume = 0;
    if (currentVolume > 21) currentVolume = 21;
    if (alarmVolume < 0) alarmVolume = 0;
    if (alarmVolume > 21) alarmVolume = 21;
    if (alarmSoundIndex < 0 || alarmSoundIndex >= NUM_ALARM_SOUNDS) alarmSoundIndex = 0;
    if ((int)alarmRepeatMode < 0 || (int)alarmRepeatMode > 4) {
        alarmRepeatMode = REPEAT_SCHOOL_SCHEDULE;
    }
    if (hueBrightness < 0) hueBrightness = 0;
    if (hueBrightness > 100) hueBrightness = 100;
    if (hueSunriseDuration != 10 && hueSunriseDuration != 20 && hueSunriseDuration != 30) hueSunriseDuration = 30;
    if (nightBrightness < 1) nightBrightness = 1;
    if (nightBrightness > 20) nightBrightness = 20;
    if (dayBrightness < 10) dayBrightness = 10;
    if (dayBrightness > 100) dayBrightness = 100;
    if ((int)wmataMode < 0 || (int)wmataMode > 2) wmataMode = WMATA_MODE_OFF;
    
    Serial.printf("[NVS] Loaded: Alarm %02d:%02d (%s, Repeat: %d), Vol: %d, AlarmVol: %d, Sound: %d, BK: %s, Gentle: %s, NightDim: %d%%, DayBri: %d%%\r\n", 
                  alarmHour, alarmMinute, alarmEnabled ? "ON" : "OFF", (int)alarmRepeatMode, currentVolume, 
                  alarmVolume, alarmSoundIndex, backlightState ? "ON" : "OFF", gentleWakeEnabled ? "ON" : "OFF",
                  nightBrightness, dayBrightness);
    nvsUnlock();
}

void saveSettings() {
    nvsLock();
    preferences.begin("alarm_clock", false); // read-write
    preferences.putInt("hour", alarmHour);
    preferences.putInt("minute", alarmMinute);
    preferences.putBool("enabled", alarmEnabled);
    preferences.putInt("volume", currentVolume);
    preferences.putInt("alm_vol", alarmVolume);
    preferences.putInt("alm_snd", alarmSoundIndex);
    preferences.putBool("bklight", backlightState);
    preferences.putBool("gentle", gentleWakeEnabled);
    preferences.putInt("alm_rpt", (int)alarmRepeatMode);
    
    // hueLightsEnabled and hueBrightness are live bridge state; do not persist to NVS
    preferences.putBool("hue_wake", hueSunriseEnabled);
    preferences.putInt("hue_dur", hueSunriseDuration);
    preferences.putString("hue_ip", hueBridgeIP);
    preferences.putString("hue_user", hueUsername);
    preferences.putString("hue_scid", activeHueSceneID);
    preferences.putString("hue_scnm", activeHueSceneName);
    preferences.putBool("weather", weatherEnabled);
    preferences.putBool("sch_cal_en", schoolCalendarEnabled);
    preferences.putBool("auto_dim", autoDimmingEnabled);
    preferences.putInt("night_big", nightBigClock);
    preferences.putInt("night_bri", nightBrightness);
    preferences.putInt("day_bri", dayBrightness);
    preferences.putBool("rem_on", reminderEnabled);
    preferences.putInt("rem_hr", reminderHour);
    preferences.putInt("rem_min", reminderMinute);
    preferences.putString("rem_txt", reminderText);
    preferences.putString("rem_snd", reminderSound);
    for (int i = 0; i < MAX_COUNTDOWN_EVENTS; i++) {
        char enKey[12]; snprintf(enKey, sizeof(enKey), "cnt_%d_en", i);
        char nameKey[16]; snprintf(nameKey, sizeof(nameKey), "cnt_%d_name", i);
        char yKey[10]; snprintf(yKey, sizeof(yKey), "cnt_%d_y", i);
        char mKey[10]; snprintf(mKey, sizeof(mKey), "cnt_%d_m", i);
        char dKey[10]; snprintf(dKey, sizeof(dKey), "cnt_%d_d", i);
        preferences.putBool(enKey, countdownEvents[i].enabled);
        preferences.putString(nameKey, countdownEvents[i].name);
        preferences.putInt(yKey, countdownEvents[i].year);
        preferences.putInt(mKey, countdownEvents[i].month);
        preferences.putInt(dKey, countdownEvents[i].day);
    }
    
    preferences.putString("wmata_key", wmataApiKey);
    preferences.putString("wmata_stop", wmataStopId);
    preferences.putString("wmata_route", wmataRouteFilter);
    preferences.putInt("wmata_mode", (int)wmataMode);

    for (int i = 0; i < 15; i++) {
        String key = "affirm" + String(i);
        preferences.putString(key.c_str(), affirmations[i]);
    }

    preferences.end();
    nvsUnlock();
    Serial.println("[NVS] Settings saved.");
}

void initializeEditVariables() {
    editSleepMinutes = sleepDurationMinutes;
    if (isCountdownTimerActive()) {
        editTimerMinutes = (getTimerSnapshot().remainingSeconds + 59) / 60;
    } else {
        editTimerMinutes = 15;
    }
    editBacklight = backlightState;
    editNightBrightness = nightBrightness;
    editDayBrightness = dayBrightness;
    editAlarmEnabled = alarmEnabled;
    editAlarmVol = alarmVolume;
    editAlarmSound = alarmSoundIndex;
    editGentleWake = gentleWakeEnabled;
    editSpotifyShuffleState = getSpotifyShuffleState();
    
    editHueLightsEnabled = hueLightsEnabled;
    editHueBrightness = hueBrightness;
    editHueSunriseEnabled = hueSunriseEnabled;
    editHueSunriseDuration = hueSunriseDuration;
    
    editHueSceneIndex = 0;
    for (int i = 0; i < numHueScenes; i++) {
        if (hueScenes[i].id == activeHueSceneID) {
            editHueSceneIndex = i;
            break;
        }
    }
    editWeatherEnabled = weatherEnabled;
    editSchoolCalendar = schoolCalendarEnabled;
    editNightBigClock = nightBigClock;
    editAlarmRepeatMode = alarmRepeatMode;
    editAlarmHour = alarmHour;
    editAlarmMinute = alarmMinute;
    editAutoDimming = autoDimmingEnabled;
    editWmataMode = wmataMode;
    editDeviceVolume = currentVolume;
}

void saveSettingValue(int item) {
    switch (currentMenuLevel) {
        case LEVEL_MAIN:
            break;
            
        case LEVEL_ALARM_CONFIG:
            switch (item) {
                case 0: // Alarm Enable
                    alarmEnabled = editAlarmEnabled;
                    break;
                case 1: // Alarm Hour
                    alarmHour = editAlarmHour;
                    break;
                case 2: // // Alarm Minute
                    alarmMinute = editAlarmMinute;
                    break;
                case 3: // Repeat Mode
                    alarmRepeatMode = editAlarmRepeatMode;
                    break;
                case 4: // Alarm Sound
                    alarmSoundIndex = editAlarmSound;
                    break;
                case 5: // Alarm Vol
                    alarmVolume = editAlarmVol;
                    break;
                case 6: // Gentle Wake
                    gentleWakeEnabled = editGentleWake;
                    break;
            }
            break;
            
        case LEVEL_SPOTIFY_SETTINGS:
            switch (item) {
                case 0: { // Spotify Devices Connect
                    String devId = getSpotifyDeviceId(getSpotifySelectedDeviceIndex());
                    String devName = getSpotifyDeviceName(getSpotifySelectedDeviceIndex());
                    if (devId.length() > 0) {
                        Serial.printf("[Spotify] Connecting/transferring playback to: %s (%s)\r\n", devName.c_str(), devId.c_str());
                        displayLines("Transferring to", devName);
                        SpotifyCommand cmd = {SPOTIFY_CMD_SELECT_DEVICE, 0, false, ""};
                        strncpy(cmd.stringParam, devId.c_str(), sizeof(cmd.stringParam) - 1);
                        postSpotifyCommand(cmd);
                        delay(1500);
                        currentState = STATE_MUSIC_MODE;
                        lastLCDUpdate = 0;
                    }
                    break;
                }
                case 1: { // Spotify Playlist
                    String plUri = getSpotifyPlaylistUri(getSpotifySelectedPlaylistIndex());
                    String plName = getSpotifyPlaylistName(getSpotifySelectedPlaylistIndex());
                    if (plUri.length() > 0) {
                        Serial.printf("[Spotify] Starting playlist context: %s (%s)\r\n", plName.c_str(), plUri.c_str());
                        displayLines("Play Playlist:", plName);
                        SpotifyCommand cmd = {SPOTIFY_CMD_PLAY_PLAYLIST, 0, false, ""};
                        strncpy(cmd.stringParam, plUri.c_str(), sizeof(cmd.stringParam) - 1);
                        postSpotifyCommand(cmd);
                        SpotifyCommand shufCmd = {SPOTIFY_CMD_SET_SHUFFLE, 0, getSpotifyShuffleState(), ""};
                        postSpotifyCommand(shufCmd);
                        delay(1500);
                        currentState = STATE_MUSIC_MODE;
                        lastLCDUpdate = 0;
                    }
                    break;
                }
                case 2: { // Spotify Shuffle
                    setSpotifyShuffleState(editSpotifyShuffleState);
                    SpotifyCommand cmd = {SPOTIFY_CMD_SET_SHUFFLE, 0, editSpotifyShuffleState, ""};
                    postSpotifyCommand(cmd);
                }
                    break;
                case 3: { // Exit Music Mode
                    Serial.println("[Spotify] Exit Music Mode selected from menu.");
                    SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
                    postSpotifyCommand(cmd);
                    setSpotifyIsPlaying(false);
                    enteredSettingsFromMusicMode = false;
                    currentState = STATE_IDLE_CLOCK;
                    spotifyPausedStartTime = 0;
                    exitMenu(false);
                }
                    break;
                case 4: // Back to main menu
                    currentMenuLevel = LEVEL_MAIN;
                    currentMenuItem = 0;
                    break;
            }
            break;
            
        case LEVEL_HUE_LIGHTS:
            switch (item) {
                case 0: // Lights ON/OFF
                    hueLightsEnabled = editHueLightsEnabled;
                    updateHueLightState();
                    break;
                case 1: // Brightness
                    hueBrightness = editHueBrightness;
                    activeHueSceneID = ""; // Clear scene on manual brightness change
                    activeHueSceneName = "None";
                    updateHueLightState();
                    break;
                case 2: // Hue Scene
                    if (numHueScenes > 0 && editHueSceneIndex >= 0 && editHueSceneIndex < numHueScenes) {
                        activeHueSceneID = hueScenes[editHueSceneIndex].id;
                        activeHueSceneName = hueScenes[editHueSceneIndex].name;
                        // If light is currently ON, trigger the scene immediately
                        if (hueLightsEnabled) {
                            updateHueLightState();
                        }
                    } else {
                        activeHueSceneID = "";
                        activeHueSceneName = "None";
                    }
                    break;
                case 3: // Sunrise Wake
                    hueSunriseEnabled = editHueSunriseEnabled;
                    break;
                case 4: // Sunrise Duration
                    hueSunriseDuration = editHueSunriseDuration;
                    break;
            }
            break;
            
        case LEVEL_DISPLAY_CONFIG:
            switch (item) {
                case 0: // LCD Backlight
                    backlightState = editBacklight;
                    setBacklight(backlightState);
                    break;
                case 1: // Auto Night Dim
                    autoDimmingEnabled = editAutoDimming;
                    break;
                case 2: // Night Big Clock
                    nightBigClock = editNightBigClock;
                    break;
                case 3: // Show Weather
                    weatherEnabled = editWeatherEnabled;
                    break;
                case 4: // Show School Calendar
                    schoolCalendarEnabled = editSchoolCalendar;
                    break;
                case 5: // WMATA Bus Mode
                    wmataMode = editWmataMode;
                    invalidateWmataCache();
                    break;
            }
            break;

        case LEVEL_AUDIO_SETTINGS:
            switch (item) {
                case 0: // Device Volume
                    currentVolume = editDeviceVolume;
                    updateAudioVolume();
                    break;
                case 1: // Bedside Timer
                    if (editTimerMinutes > 0) {
                        startCountdownTimer(editTimerMinutes);
                        clockScreenIndex = 1;
                        screenCarouselTimeout = millis() + 10000;
                        Serial.printf("[Settings] Bedside timer started for %d minutes.\r\n", editTimerMinutes);
                    } else {
                        stopCountdownTimer();
                        Serial.println("[Settings] Bedside timer stopped.");
                    }
                    break;
                case 2: // Sleep Timer
                    sleepDurationMinutes = editSleepMinutes;
                    if (sleepDurationMinutes > 0) {
                        sleepTimerEnd = millis() + (sleepDurationMinutes * 60 * 1000);
                        Serial.printf("[Settings] Sleep timer set to %d minutes.\r\n", sleepDurationMinutes);
                        if (hueLightsEnabled) {
                            startHueSleepTimerFade(sleepDurationMinutes);
                        }
                    } else {
                        sleepTimerEnd = 0;
                        Serial.println("[Settings] Sleep timer disabled.");
                    }
                    break;
                case 3: // Noise Machine action, no setting to save
                    break;
            }
            break;
            
    }
    // RAM settings updated; NVS flash batch commit occurs on menu exit
}

int getCurrentMenuSize() {
    int size = 0;
    getMenuDescriptor(currentMenuLevel, size);
    return size;
}

void enterMenu() {
    initializeEditVariables();
    currentState = STATE_SETTINGS_MENU;
    currentMenuState = MENU_NAVIGATING;
    currentMenuLevel = LEVEL_MAIN;
    currentMenuItem = 0;
    menuInactivityTimeout = millis() + 30000;
    lastLCDUpdate = 0;
    Serial.println("[Menu] Entered Settings Menu.");
}

void exitMenu(bool save) {
    // If there was an active audio preview, stop it
    if (previewStopTimeout > 0) {
        previewStopTimeout = 0;
        stopAudioPlayback();
    }

    if (save) {
        saveSettings();
    }
    
    if (enteredSettingsFromMusicMode) {
        enteredSettingsFromMusicMode = false;
        currentState = STATE_MUSIC_MODE;
    } else {
        currentState = STATE_IDLE_CLOCK;
    }
    currentMenuState = MENU_NAVIGATING;
    currentMenuLevel = LEVEL_MAIN;
    currentMenuItem = 0;
    lastLCDUpdate = 0;
    lastCreatedCharScreenIndex = -1; // Force re-creating any custom glyphs for idle clock
    Serial.println("[Menu] Exited Settings Menu.");
}

void handleMenuSelect() {
    menuInactivityTimeout = millis() + 30000; // Reset inactivity timeout on any button press
    
    switch (currentMenuLevel) {
        case LEVEL_MAIN:
            switch (currentMenuItem) {
                case 0: // Alarm Config
                    initializeEditVariables();
                    currentMenuLevel = LEVEL_ALARM_CONFIG;
                    currentMenuItem = 0;
                    break;
                case 1: // Spotify Settings
                    initializeEditVariables();
                    currentMenuLevel = LEVEL_SPOTIFY_SETTINGS;
                    currentMenuItem = 0;
                    break;
                case 2: // Audio Settings
                    initializeEditVariables();
                    currentMenuLevel = LEVEL_AUDIO_SETTINGS;
                    currentMenuItem = 0;
                    break;
                case 3: // Hue Lights
                    displayLines("Fetching Hue...", "Please wait...");
                    fetchHueLightState();
                    fetchHueScenes();
                    initializeEditVariables();
                    currentMenuLevel = LEVEL_HUE_LIGHTS;
                    currentMenuItem = 0;
                    break;
                case 4: // Display Config
                    initializeEditVariables();
                    currentMenuLevel = LEVEL_DISPLAY_CONFIG;
                    currentMenuItem = 0;
                    break;
                case 5: // Device Info
                    currentMenuLevel = LEVEL_DEVICE_INFO;
                    currentMenuItem = 0;
                    break;
                case 6: // Exit
                    exitMenu(true);
                    break;
            }
            break;

        case LEVEL_AUDIO_SETTINGS: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 2; // Audio Settings is index 2 on main menu
            } else if (currentMenuItem == 0 || currentMenuItem == 1 || currentMenuItem == 2) {
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            } else if (currentMenuItem == 3) { // Noise Machine
                Serial.println("[Menu] Starting Noise Machine Mode...");
                Serial.println("CMD:NOISE_ON");
                currentState = STATE_NOISE_MACHINE;
                startAudioPlayback("/brown_noise.wav", true);
            }
            break;
        }

        case LEVEL_DEVICE_INFO: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 5; // Device Info is index 5 on main menu
            }
            break;
        }

        case LEVEL_DISPLAY_CONFIG: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 4; // Display is index 4 on main menu
            } else {
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            }
            break;
        }
            
        case LEVEL_ALARM_CONFIG: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 0; // Alarm is index 0
            } else {
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            }
            break;
        }
            
        case LEVEL_SPOTIFY_SETTINGS: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 1; // Spotify is index 1 on main menu
            } else if (currentMenuItem == 3) { // Exit Music
                Serial.println("[Spotify] Exit Music selected from menu.");
                {
                    SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
                    postSpotifyCommand(cmd);
                }
                setSpotifyIsPlaying(false);
                enteredSettingsFromMusicMode = false;
                currentState = STATE_IDLE_CLOCK;
                spotifyPausedStartTime = 0;
                exitMenu(false);
            } else if (currentMenuItem == 0) { // Target Device
                {
                    SpotifyCommand cmd = {SPOTIFY_CMD_FETCH_DEVICES, 0, false, ""};
                    postSpotifyCommand(cmd);
                }
                displayLines("Spotify Conn.", "Fetching...");
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
                menuInactivityTimeout = millis() + 30000;
            } else if (currentMenuItem == 1) { // Select Playlist
                {
                    SpotifyCommand cmd = {SPOTIFY_CMD_FETCH_PLAYLISTS, 0, false, ""};
                    postSpotifyCommand(cmd);
                }
                displayLines("Spotify Play.", "Fetching...");
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
                menuInactivityTimeout = millis() + 30000;
            } else if (currentMenuItem == 2) { // Spotify Shuffle
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            }
            break;
        }
            
        case LEVEL_HUE_LIGHTS: {
            int size = 0;
            const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
            if (menu && currentMenuItem >= 0 && currentMenuItem < size && menu[currentMenuItem].type == MENU_TYPE_BACK) {
                currentMenuLevel = LEVEL_MAIN;
                currentMenuItem = 3; // Hue Lights is index 3 on main menu
            } else if (currentMenuItem == 2) { // Hue Scene
                displayLines("Hue Scenes...", "Fetching...");
                fetchHueScenes();
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            } else {
                currentMenuState = MENU_EDITING;
                initializeEditVariables();
            }
            break;
        }
    }
}

void snoozeAlarm(int minutes) {
    Serial.printf("[Alarm] Snoozed for %d minutes.\r\n", minutes);
    stopAudioPlayback();
    
    // Add snooze offset to alarm time
    time_t t_now = time(nullptr);
    struct tm *tmp = localtime(&t_now);
    tmp->tm_min += minutes;
    mktime(tmp); // normalize tm struct
    
    // Set transient snooze target
    snoozeHour = tmp->tm_hour;
    snoozeMinute = tmp->tm_min;
    snoozeActive = true;
    
    char snoozeMsg[17];
    snprintf(snoozeMsg, sizeof(snoozeMsg), "  Snooze: %2dm   ", minutes);
    displayLines(" Alarm Snoozed  ", snoozeMsg);
    delay(2500);
    
    currentState = STATE_IDLE_CLOCK;
    lastLCDUpdate = 0;
}

void dismissUpcomingAlarmToday() {
    alarmDismissedForToday = true;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        lastAlarmDismissedDay = timeinfo.tm_yday;
        nvsLock();
        preferences.begin("alarm_clock", false);
        preferences.putInt("last_dism_day", lastAlarmDismissedDay);
        preferences.end();
        nvsUnlock();
        Serial.printf("[Alarm] Latching today's alarm dismissal to NVS (day %d)\r\n", lastAlarmDismissedDay);
    }
}

void dismissAlarm() {
    Serial.println("[Alarm] Alarm dismissed.");
    stopAudioPlayback();
    snoozeActive = false;
    dismissUpcomingAlarmToday();
    
    // If repeat mode is REPEAT_ONCE, disable alarm
    if (alarmRepeatMode == REPEAT_ONCE) {
        alarmEnabled = false;
        saveSettings();
    } else {
        // Restore user's last manual volume from NVS
        loadSettings(); 
    }
    
    // Dynamically load the Heart icon to Slot 7
    setHeartCustomChar();
    
    int randIdx = random(15);

    // Show positive reinforcement message
    displayLines("Alarm Dismissed ", affirmations[randIdx]);
    delay(3500); // Allow time to read
    
    // Reset weather indicators tracking so slot 7 is recreated next loop
    lastCreatedCharScreenIndex = -1;
    
    currentState = STATE_IDLE_CLOCK;
    lastLCDUpdate = 0;
}

void startAlarmPlayback() {
    snoozeDurationSelection = 0; // Default snooze is "Dismiss" (0)
    
    if (gentleWakeEnabled) {
        gentleWakeStartTime = millis();
        gentleWakeActive = true;
        // Start playing at volume 0
        currentVolume = 0;
        updateAudioVolume();
    } else {
        gentleWakeActive = false;
        currentVolume = alarmVolume;
        updateAudioVolume();
    }
    
    startAudioPlayback(ALARM_SOUNDS[alarmSoundIndex].path, true);
}

void adjustSettingValue(int itemIdx, int delta) {
    int size = 0;
    const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
    if (!menu || itemIdx < 0 || itemIdx >= size) return;

    const MenuItemDescriptor& item = menu[itemIdx];

    // Special dynamic scroll handlers
    if (currentMenuLevel == LEVEL_SPOTIFY_SETTINGS && itemIdx == 0) {
        if (numSpotifyDevices > 0) {
            selectedSpotifyDeviceIndex += delta;
            if (selectedSpotifyDeviceIndex < 0) selectedSpotifyDeviceIndex = numSpotifyDevices - 1;
            if (selectedSpotifyDeviceIndex >= numSpotifyDevices) selectedSpotifyDeviceIndex = 0;
        }
        return;
    }
    if (currentMenuLevel == LEVEL_SPOTIFY_SETTINGS && itemIdx == 1) {
        if (numSpotifyPlaylists > 0) {
            selectedSpotifyPlaylistIndex += delta;
            if (selectedSpotifyPlaylistIndex < 0) selectedSpotifyPlaylistIndex = numSpotifyPlaylists - 1;
            if (selectedSpotifyPlaylistIndex >= numSpotifyPlaylists) selectedSpotifyPlaylistIndex = 0;
        }
        return;
    }
    if (currentMenuLevel == LEVEL_HUE_LIGHTS && itemIdx == 2) {
        if (numHueScenes > 0) {
            editHueSceneIndex += delta;
            if (editHueSceneIndex < 0) editHueSceneIndex = numHueScenes - 1;
            if (editHueSceneIndex >= numHueScenes) editHueSceneIndex = 0;
        }
        return;
    }

    if (item.type == MENU_TYPE_TOGGLE && item.targetVar) {
        bool* b = (bool*)item.targetVar;
        *b = !(*b);
    } else if (item.type == MENU_TYPE_SELECT && item.targetVar) {
        int* val = (int*)item.targetVar;
        int step = (item.step > 0) ? item.step : 1;
        *val += delta * step;
        if (*val < item.minVal) *val = item.maxVal;
        if (*val > item.maxVal) *val = item.minVal;
    } else if (item.type == MENU_TYPE_NUMBER && item.targetVar) {
        int* val = (int*)item.targetVar;
        int step = (item.step > 0) ? item.step : 1;
        *val += delta * step;
        if (*val < item.minVal) *val = item.minVal;
        if (*val > item.maxVal) *val = item.maxVal;
    }

    // Side effects on specific menu edits
    if (currentMenuLevel == LEVEL_ALARM_CONFIG && itemIdx == 4) {
        previewAlarmSound(editAlarmSound);
    } else if (currentMenuLevel == LEVEL_HUE_LIGHTS && itemIdx == 1) {
        hueBrightness = editHueBrightness;
        activeHueSceneID = "";
        activeHueSceneName = "None";
        updateHueLightState();
    } else if (currentMenuLevel == LEVEL_DISPLAY_CONFIG && item.targetVar == &editDayBrightness) {
        dayBrightness = editDayBrightness;
        updateHardwareBrightness();
    } else if (currentMenuLevel == LEVEL_DISPLAY_CONFIG && item.targetVar == &editNightBrightness) {
        nightBrightness = editNightBrightness;
        updateHardwareBrightness();
    } else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && itemIdx == 0) {
        currentVolume = editDeviceVolume;
        isMuted = false;
        updateAudioVolume();
    }
}

void adjustSnoozeSelection(int delta) {
    snoozeDurationSelection += delta * 5;
    if (snoozeDurationSelection < 0) snoozeDurationSelection = 0;   // Stops cleanly at Dismiss (0) on counter-clockwise
    if (snoozeDurationSelection > 15) snoozeDurationSelection = 15; // Stops at 15m on clockwise
}

static String centerBrackets(String text) {
    String formatted = "[ " + text + " ]";
    int spacesNeeded = 16 - formatted.length();
    if (spacesNeeded <= 0) return formatted.substring(0, 16);
    
    int leftSpaces = spacesNeeded / 2;
    int rightSpaces = spacesNeeded - leftSpaces;
    
    String l = "";
    for (int i = 0; i < leftSpaces; i++) l += " ";
    String r = "";
    for (int i = 0; i < rightSpaces; i++) r += " ";
    
    return l + formatted + r;
}

void renderSettingsMenu() {
    String l1 = "";
    String l2 = "";

    int size = 0;
    const MenuItemDescriptor* menu = getMenuDescriptor(currentMenuLevel, size);
    if (!menu || currentMenuItem < 0 || currentMenuItem >= size) return;

    const MenuItemDescriptor& item = menu[currentMenuItem];
    l1 = item.label;

    if (currentMenuLevel == LEVEL_DEVICE_INFO && currentMenuItem < 4) {
        // Special dynamic strings for Device Info screen
        if (currentMenuItem == 0) {
            int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
            int quality = (rssi <= -100) ? 0 : ((rssi >= -50) ? 100 : 2 * (rssi + 100));
            const char* sigLabel = (rssi >= -50) ? "Exc" : ((rssi >= -65) ? "Good" : ((rssi >= -75) ? "Fair" : "Weak"));
            l1 = "1. WiFi: " + String(quality) + "% " + String(sigLabel);
            l2 = "Net: " + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Offline");
            if (l2.length() > 16) l2 = l2.substring(0, 16);
        } else if (currentMenuItem == 1) {
            l1 = "2. IP Address";
            l2 = "  " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Not Connected");
            if (l2.length() > 16) l2 = l2.substring(0, 16);
        } else if (currentMenuItem == 2) {
            l1 = "3. Version Info";
            l2 = "  Ver: " FIRMWARE_VERSION;
        } else if (currentMenuItem == 3) {
            setHeartCustomChar();
            l1 = "4. Created For";
            static int fayeScrollIndex = 0;
            String fayeMsg = "Built with \x07 for Faye    ";
            String extendedFaye = fayeMsg + fayeMsg;
            l2 = extendedFaye.substring(fayeScrollIndex, fayeScrollIndex + 16);
            fayeScrollIndex = (fayeScrollIndex + 1) % fayeMsg.length();
        }
        displayLines(l1, l2);
        return;
    }

    if (currentMenuState == MENU_NAVIGATING) {
        if (item.type == MENU_TYPE_SUBMENU) {
            l2 = "[Enter Config]";
        } else if (item.type == MENU_TYPE_BACK) {
            l2 = "[To Main Menu]";
        } else if (item.type == MENU_TYPE_ACTION) {
            if (currentMenuLevel == LEVEL_MAIN && currentMenuItem == 6) l2 = "[Push to Exit]";
            else if (currentMenuLevel == LEVEL_SPOTIFY_SETTINGS && currentMenuItem == 3) l2 = "[Return Clock]";
            else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && currentMenuItem == 3) l2 = "[Push to Start]";
            else if (currentMenuLevel == LEVEL_HUE_LIGHTS && currentMenuItem == 2) {
                l2 = "  Cur: " + (activeHueSceneName.isEmpty() ? "None" : activeHueSceneName);
            }
            else l2 = "[Push Select]";
        } else if (item.type == MENU_TYPE_TOGGLE && item.targetVar) {
            bool val = *(bool*)item.targetVar;
            l2 = String("  Cur: ") + (val ? "ON" : "OFF");
        } else if (item.type == MENU_TYPE_SELECT && item.selectOptions && item.targetVar) {
            int val = *(int*)item.targetVar;
            if (val >= 0 && val < item.numSelectOptions) {
                l2 = String("  Cur: ") + item.selectOptions[val];
            } else {
                l2 = "  Cur: " + String(val);
            }
        } else if (item.type == MENU_TYPE_NUMBER && item.targetVar) {
            int val = *(int*)item.targetVar;
            if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 1) { // Alarm Hour
                auto format12H = [](int hr) -> String {
                    if (hr == 0) return String("12 AM");
                    if (hr == 12) return String("12 PM");
                    if (hr < 12) return String(hr) + " AM";
                    return String(hr - 12) + " PM";
                };
                l2 = "  Cur: " + format12H(val);
            } else if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 2) { // Alarm Min
                char minBuf[6];
                snprintf(minBuf, sizeof(minBuf), "%02d", val);
                l2 = "  Cur: " + String(minBuf);
            } else if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 4) { // Alarm Sound
                l2 = "  Cur: " + String(ALARM_SOUNDS[val].name);
            } else if (currentMenuLevel == LEVEL_HUE_LIGHTS && currentMenuItem == 4) { // Sunrise Dur
                l2 = "  Cur: " + String(val) + "m";
            } else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && currentMenuItem == 1) { // Bedside Timer
                l2 = isCountdownTimerActive() ? ("  Cur: " + getTimerSnapshot().timeString) : "  Cur: Off";
            } else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && currentMenuItem == 2) { // Sleep Timer
                l2 = (val == 0) ? "  Cur: Off" : ("  Cur: " + String(val) + "m");
            } else if (item.maxVal == 100) {
                l2 = "  Cur: " + String(val) + "%";
            } else {
                l2 = "  Cur: " + String(val);
            }
        }
    } else if (currentMenuState == MENU_EDITING) {
        if (currentMenuLevel == LEVEL_HUE_LIGHTS && currentMenuItem == 2) {
            if (numHueScenes > 0) {
                String scName = hueScenes[editHueSceneIndex].name;
                l2 = centerBrackets(scName);
            } else {
                l2 = "[ No Scenes ]";
            }
        } else if (currentMenuLevel == LEVEL_SPOTIFY_SETTINGS && currentMenuItem == 0) {
            if (isFetchingDevices) l2 = "  Fetching...   ";
            else if (getSpotifyNumDevices() == 0) l2 = "[ No Devices ]  ";
            else {
                String name = getSpotifyDeviceName(getSpotifySelectedDeviceIndex());
                l2 = centerBrackets(name);
            }
        } else if (currentMenuLevel == LEVEL_SPOTIFY_SETTINGS && currentMenuItem == 1) {
            if (isFetchingPlaylists) l2 = "  Fetching...   ";
            else if (getSpotifyNumPlaylists() == 0) l2 = "[ No Playlists ]";
            else {
                String name = getSpotifyPlaylistName(getSpotifySelectedPlaylistIndex());
                l2 = centerBrackets(name);
            }
        } else if (item.type == MENU_TYPE_TOGGLE && item.targetVar) {
            bool val = *(bool*)item.targetVar;
            l2 = centerBrackets(val ? "ON" : "OFF");
        } else if (item.type == MENU_TYPE_SELECT && item.selectOptions && item.targetVar) {
            int val = *(int*)item.targetVar;
            if (val >= 0 && val < item.numSelectOptions) {
                l2 = centerBrackets(item.selectOptions[val]);
            } else {
                l2 = centerBrackets(String(val));
            }
        } else if (item.type == MENU_TYPE_NUMBER && item.targetVar) {
            int val = *(int*)item.targetVar;
            if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 1) {
                auto format12H = [](int hr) -> String {
                    if (hr == 0) return String("12 AM");
                    if (hr == 12) return String("12 PM");
                    if (hr < 12) return String(hr) + " AM";
                    return String(hr - 12) + " PM";
                };
                l2 = centerBrackets(format12H(val));
            } else if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 2) {
                char minBuf[6];
                snprintf(minBuf, sizeof(minBuf), "%02d", val);
                l2 = centerBrackets(String(minBuf));
            } else if (currentMenuLevel == LEVEL_ALARM_CONFIG && currentMenuItem == 4) {
                l2 = centerBrackets(ALARM_SOUNDS[val].name);
            } else if (currentMenuLevel == LEVEL_HUE_LIGHTS && currentMenuItem == 4) {
                l2 = centerBrackets(String(val) + " min");
            } else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && currentMenuItem == 1) { // Bedside Timer
                l2 = (val == 0) ? centerBrackets("Off (Stop)") : centerBrackets(String(val) + " min");
            } else if (currentMenuLevel == LEVEL_AUDIO_SETTINGS && currentMenuItem == 2) { // Sleep Timer
                l2 = (val == 0) ? centerBrackets("Off") : centerBrackets(String(val) + " min");
            } else if (item.maxVal == 100 || item.maxVal == 21) {
                char slider[17];
                slider[0] = '[';
                int numBlocks = (val * 14) / item.maxVal;
                for (int i = 1; i <= 14; i++) {
                    if (i <= numBlocks) slider[i] = (char)0xFF;
                    else slider[i] = '-';
                }
                slider[15] = ']';
                slider[16] = '\0';
                l2 = String(slider);
            } else {
                l2 = centerBrackets(String(val));
            }
        }
    }

    displayLines(l1, l2);
}
