#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include "globals.h"

// Alarm & settings variables (persisted to NVS)
extern int alarmHour;
extern int alarmMinute;
extern bool alarmEnabled;
extern int alarmVolume;
extern int alarmSoundIndex;
extern bool backlightState;
extern bool gentleWakeEnabled;
extern bool autoDimmingEnabled;

enum AlarmRepeatMode {
    REPEAT_ONCE = 0,
    REPEAT_EVERYDAY = 1,
    REPEAT_WEEKDAYS = 2,
    REPEAT_WEEKENDS = 3,
    REPEAT_SCHOOL_SCHEDULE = 4
};
extern AlarmRepeatMode alarmRepeatMode;
extern AlarmRepeatMode editAlarmRepeatMode;

// Non-blocking timers & display states
extern unsigned long lastLCDUpdate;
extern unsigned long volumeDisplayTimeout;
extern unsigned long menuInactivityTimeout;
extern bool enteredSettingsFromMusicMode;

// Menu Item Types for Data-Driven Table Rendering
enum MenuItemType {
    MENU_TYPE_SUBMENU = 0,
    MENU_TYPE_TOGGLE = 1,
    MENU_TYPE_NUMBER = 2,
    MENU_TYPE_SELECT = 3,
    MENU_TYPE_ACTION = 4,
    MENU_TYPE_BACK = 5
};

struct MenuItemDescriptor {
    const char* label;
    MenuItemType type;
    int targetLevel;
    void* targetVar;
    int minVal;
    int maxVal;
    int step;
    const char* const* selectOptions;
    int numSelectOptions;
};

// Menu State
extern MenuState currentMenuState;
extern MenuLevel currentMenuLevel;
extern int currentMenuItem;

// Temporary submenu variables being edited
extern int editSleepMinutes;
extern int editTimerMinutes;
extern bool editBacklight;
extern bool editAlarmEnabled;
extern int editAlarmVol;
extern int editAlarmSound;
extern bool editGentleWake;
extern bool editWeatherEnabled;
extern bool editSchoolCalendar;
extern int editNightBigClock;
extern bool editAutoDimming;
extern int editAlarmHour;
extern int editAlarmMinute;
extern int editHueSceneIndex;
extern WmataMode editWmataMode;

// Morning affirmations
extern String affirmations[15];

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

// Function declarations
void loadSettings();
void saveSettings();
void renderSettingsMenu();
void handleMenuSelect();
void adjustSettingValue(int item, int delta);
void initializeEditVariables();
int getCurrentMenuSize();
void saveSettingValue(int item);
void enterMenu();
void exitMenu(bool save);
void snoozeAlarm(int minutes);
void dismissAlarm();
void dismissUpcomingAlarmToday();
void startAlarmPlayback();
void adjustSnoozeSelection(int delta);
void previewAlarmSound(int soundIndex);

#endif // SETTINGS_MENU_H
