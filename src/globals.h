#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "config.h"
#include "wmata_client.h"

#ifdef SIMULATION_MODE
// --- Mock Audio Class for Wokwi Simulation ---
class Audio {
public:
    Audio() : _running(false) {}
    void setPinout(uint8_t bclk, uint8_t lrck, uint8_t dout) {
        Serial.printf("[Mock Audio] setPinout BCLK=%d, LRCK=%d, DOUT=%d\r\n", bclk, lrck, dout);
    }
    void setVolume(uint8_t vol) {
        Serial.printf("[Mock Audio] setVolume=%d\r\n", vol);
    }
    bool setFileLoop(bool input) {
        Serial.printf("[Mock Audio] setFileLoop=%d\r\n", input);
        return true;
    }
    void connecttohost(const char* url) {
        Serial.printf("[Mock Audio] connecttohost: %s\r\n", url);
        _running = true;
    }
    template <typename T>
    bool connecttoFS(T& fs, const char* path) {
        Serial.printf("[Mock Audio] connecttoFS: %s\r\n", path);
        _running = true;
        return true;
    }
    void stopSong() {
        Serial.println("[Mock Audio] stopSong");
        _running = false;
    }
    bool isRunning() {
        return _running;
    }
    void loop() {
        // Mock loop does nothing
    }
private:
    bool _running;
};
#else
// --- Real Audio library for physical hardware ---
#include <Audio.h>
#endif

// ==========================================
// System States & Gestures
// ==========================================
enum SystemState {
    STATE_DISCONNECTED,
    STATE_SYNCING_TIME,
    STATE_IDLE_CLOCK,
    STATE_MUSIC_MODE,
    STATE_ALARM_RINGING,
    STATE_SETTINGS_MENU,
    STATE_NOISE_MACHINE,
    STATE_ALERT_MESSAGE,
    STATE_VOICE_MEMO_PROMPT,
    STATE_SUNRISE_CANCEL_PROMPT,
    STATE_SNOOZE_CANCEL_PROMPT,
    STATE_OFFLINE_PROMPT,
    STATE_OFFLINE_SET_HOUR,
    STATE_OFFLINE_SET_MIN,
    STATE_QUICK_ACTIONS
};

enum QuickActionMode {
    QUICK_ACTION_MAIN,
    QUICK_ACTION_DEVICE_SELECT,
    QUICK_ACTION_PLAYLIST_SELECT,
    QUICK_ACTION_SLEEP_TIMER,
    QUICK_ACTION_TIMER_SELECT
};

enum QuickActionPending {
    QUICK_PENDING_NONE,
    QUICK_PENDING_PLAY,
    QUICK_PENDING_PLAYLIST
};

enum ButtonGesture {
    GESTURE_NONE,
    GESTURE_SINGLE,
    GESTURE_DOUBLE,
    GESTURE_LONG
};

enum MenuState {
    MENU_NAVIGATING,
    MENU_EDITING
};

enum MenuLevel {
    LEVEL_MAIN,
    LEVEL_ALARM_CONFIG,
    LEVEL_SPOTIFY_SETTINGS,
    LEVEL_HUE_LIGHTS,
    LEVEL_DISPLAY_CONFIG,
    LEVEL_AUDIO_SETTINGS,
    LEVEL_DEVICE_INFO
};

// ==========================================
// Shared Globals (Declared extern)
// ==========================================
extern SystemState currentState;
extern Audio audio;
extern Preferences preferences;
extern String intercomMessage;
extern String alertHeader;
extern bool messageActive;
extern bool sunriseActive;
extern unsigned long lastWifiReconnectAttempt;
extern bool alarmDismissedForToday;
extern int lastAlarmDismissedDay;

// Alarm & Settings state variables (persisted to NVS)
extern int alarmHour;
extern int alarmMinute;
extern bool alarmEnabled;
extern int alarmVolume;
extern int alarmSoundIndex;
extern bool backlightState;
extern bool gentleWakeEnabled;
extern bool autoDimmingEnabled;
extern int currentVolume;
extern bool isMuted;
extern int nightBrightness;
extern int dayBrightness;
extern int sunriseHour;
extern int sunriseMin;
extern int sunsetHour;
extern int sunsetMin;
extern bool sunTimesFetched;

// Non-blocking timers & display states
extern unsigned long lastLCDUpdate;
extern unsigned long volumeDisplayTimeout;
extern unsigned long pendingSaveSettingsTime;
extern unsigned long menuInactivityTimeout;
extern unsigned long temporaryBacklightTimeout;
extern unsigned long lastWebActivityTime;
extern bool enteredSettingsFromMusicMode;
extern int voiceMemoPromptItem;
extern unsigned long voiceMemoPromptTimeout;

// Menu State
extern MenuState currentMenuState;
extern MenuLevel currentMenuLevel;
extern int currentMenuItem;

// Quick Actions Menu State
extern QuickActionMode quickActionMode;
extern QuickActionPending quickActionPending;
extern int quickActionItem;
extern int quickActionSubItem;
extern unsigned long quickActionTimeout;

// Temporary submenu variables being edited
extern int editSleepMinutes;
extern bool editBacklight;
extern int editNightBrightness;
extern int editDayBrightness;
extern bool editAlarmEnabled;
extern int editAlarmVol;
extern int editAlarmSound;
extern bool editGentleWake;

// Active sleep timer trackers
extern unsigned long sleepTimerEnd;
extern unsigned long sleepDurationMinutes;

// Active snooze variables
extern int snoozeHour;
extern int snoozeMinute;
extern bool snoozeActive;
extern int snoozeDurationSelection;

// Gentle wake ramp trackers
extern unsigned long gentleWakeStartTime;
extern bool gentleWakeActive;

// Audio preview timer
extern unsigned long previewStopTimeout;

// Display buffering
extern String lastLine1;
extern String lastLine2;

// Weather variables
extern bool weatherEnabled;
extern int nightBigClock;
extern int weatherTemp;
extern String weatherLabel;
extern int weatherFeelsLike;
extern float weatherPrecipitationHours;
extern int weatherRainStartHour;
extern int weatherPrecipitationChance;
extern int weatherHighTemp;
extern int weatherUVIndex;
extern String weatherUVLabel;
extern int weatherCodeDaily;
extern int weatherTomorrowHighTemp;
extern int weatherTomorrowFeelsLike;
extern int weatherTomorrowUVIndex;
extern String weatherTomorrowUVLabel;
extern float weatherTomorrowPrecipitationHours;
extern int weatherTomorrowRainStartHour;
extern int weatherTomorrowPrecipChance;
extern int weatherTomorrowCodeDaily;
extern bool weatherFetched;
extern bool weatherAlertActive;
extern String weatherAlertTitle;
extern bool schoolCalendarEnabled;
extern int clockScreenIndex;
extern unsigned long screenCarouselTimeout;
extern unsigned long spotifyPausedStartTime;

// Scheduled Reminder
extern bool reminderEnabled;
extern int reminderHour;
extern int reminderMinute;
extern String reminderText;
extern String reminderSound;

// Special Calendar Countdown
#define MAX_COUNTDOWN_EVENTS 6

struct CountdownEvent {
    bool enabled;
    String name;
    int year;
    int month;
    int day;
};

extern CountdownEvent countdownEvents[MAX_COUNTDOWN_EVENTS];

#endif // GLOBALS_H
