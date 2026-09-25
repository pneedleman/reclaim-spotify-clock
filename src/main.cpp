#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_seesaw.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "globals.h"
#include "display_manager.h"
#include "audio_controller.h"
#include "hue_client.h"
#include "spotify_client.h"
#include "settings_menu.h"
#include "sinric_manager.h"
#include "weather_client.h"
#include "web_server.h"
#include "school_calendar.h"
#include "gcal_client.h"
#include "timer_manager.h"

// Define currentState
SystemState currentState = STATE_DISCONNECTED;
String intercomMessage = "";
String alertHeader = "  PARENT ALERT \x05";
bool messageActive = false;
bool sunriseActive = false;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long sunriseCancelPromptTimeout = 0;
unsigned long snoozeCancelPromptTimeout = 0;
bool alarmDismissedForToday = false;
unsigned long disconnectedStartTime = 0;
int offlineSetupHour = 12;
int offlineSetupMin = 0;

// Weather variables definition
bool weatherEnabled = true;
bool schoolCalendarEnabled = true;
int weatherTemp = 0;
String weatherLabel = "";
int weatherFeelsLike = 0;
float weatherPrecipitationHours = 0.0f;
int weatherRainStartHour = -1;
int weatherPrecipitationChance = 0;
int weatherHighTemp = 0;
int weatherUVIndex = 0;
String weatherUVLabel = "";
int weatherCodeDaily = 0;
int weatherTomorrowHighTemp = 0;
int weatherTomorrowFeelsLike = 0;
int weatherTomorrowUVIndex = 0;
String weatherTomorrowUVLabel = "Low";
float weatherTomorrowPrecipitationHours = 0.0f;
int weatherTomorrowRainStartHour = -1;
int weatherTomorrowPrecipChance = 0;
int weatherTomorrowCodeDaily = 0;
bool weatherFetched = false;
bool weatherAlertActive = false;
String weatherAlertTitle = "";
int clockScreenIndex = 0;
int lastCreatedCharScreenIndex = -1;
unsigned long screenCarouselTimeout = 0;
unsigned long spotifyPausedStartTime = 0;
unsigned long temporaryBacklightTimeout = 0;
unsigned long lastWebActivityTime = 0;

// Quick Actions Menu State
QuickActionMode quickActionMode = QUICK_ACTION_MAIN;
QuickActionPending quickActionPending = QUICK_PENDING_NONE;
int quickActionItem = 0;
int quickActionSubItem = 0;
unsigned long quickActionTimeout = 0;

// Voice Memo Prompt State
int voiceMemoPromptItem = 0;
unsigned long voiceMemoPromptTimeout = 0;

// Seesaw rotary encoder driver
#ifndef SIMULATION_MODE
Adafruit_seesaw ss;
#endif

// Button gesture configuration
const unsigned long BUTTON_DEBOUNCE_DELAY = 15; // ms (fast debounce for hardware & sim)
const unsigned long DOUBLE_CLICK_WINDOW = 500; // ms
const unsigned long LONG_PRESS_DURATION = 1000; // ms

// Button gesture trackers
bool lastButtonState = HIGH;      // Active LOW
unsigned long lastButtonDebounceTime = 0;
bool pendingSingleClick = false;
unsigned long buttonPressStartTime = 0;
unsigned long lastReleaseTime = 0;

// Hardware state trackers
int32_t lastEncoderPosition = 0;

// Non-blocking timers
unsigned long lastSeesawPoll = 0;
const unsigned long SEESAW_POLL_INTERVAL = 30; // ms (responsive volume/button)
const unsigned long LCD_UPDATE_INTERVAL = 1000; // ms (update clock every second)
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000; // ms (reconnect attempt interval)
unsigned long ntpStart = 0;
unsigned long lastHueStatePoll = 0;
const unsigned long HUE_STATE_POLL_INTERVAL = 30000; // ms (poll Hue bridge state)

// Alarm trigger tracker
bool alarmTriggeredThisMinute = false;
bool reminderTriggeredThisMinute = false;

// ==========================================
// Utility Functions
// ==========================================

// I2C Scanner to debug hardware on startup
void scanI2CBus() {
    Serial.println("\n[I2C] Scanning bus...");
    byte error, address;
    int nDevices = 0;
    for (address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if (error == 0) {
            Serial.printf("[I2C] Device found at address 0x%02X\r\n", address);
            nDevices++;
        }
    }
    if (nDevices == 0) {
        Serial.println("[I2C] No I2C devices found.\n");
    } else {
        Serial.printf("[I2C] Scan complete. Found %d device(s).\n\r\n", nDevices);
    }
}

bool seesawInitialized = false;

// Button raw state reader
bool readRawButton() {
#ifdef SIMULATION_MODE
    return (digitalRead(SIM_ENCODER_SW) == LOW);
#else
    if (!seesawInitialized) return false;
    return (ss.digitalRead(24) == LOW);
#endif
}

// Button gesture engine (debounces and detects single click, double click, long press)
ButtonGesture checkButtonGestures() {
    static bool lastRawState = false;
    static unsigned long lastDebounceTime = 0;
    static bool debouncedState = false;
    
    bool rawState = readRawButton();
    unsigned long now = millis();
    
    // Software Debounce
    if (rawState != lastRawState) {
        lastDebounceTime = now;
    }
    lastRawState = rawState;
    
    if ((now - lastDebounceTime) > BUTTON_DEBOUNCE_DELAY) {
        if (rawState != debouncedState) {
            debouncedState = rawState;
            
            if (debouncedState) {
                // Button was just pressed down
                buttonPressStartTime = now;
                if (currentState == STATE_DISCONNECTED || currentState == STATE_OFFLINE_PROMPT) {
                    Serial.println("[Button] Immediate press-down trigger for offline setup!");
                    pendingSingleClick = false;
                    return GESTURE_SINGLE;
                }
            } else {
                // Button was just released
                unsigned long pressDuration = now - buttonPressStartTime;
                if (currentState == STATE_DISCONNECTED || currentState == STATE_OFFLINE_PROMPT || 
                    currentState == STATE_OFFLINE_SET_HOUR || currentState == STATE_OFFLINE_SET_MIN) {
                    pendingSingleClick = false;
                    return GESTURE_SINGLE;
                }
                if (pressDuration >= LONG_PRESS_DURATION) {
                    return GESTURE_LONG;
                } else {
                    // Check if this release is part of a double click
                    if (pendingSingleClick && (now - lastReleaseTime <= DOUBLE_CLICK_WINDOW)) {
                        pendingSingleClick = false;
                        return GESTURE_DOUBLE;
                    } else {
                        pendingSingleClick = true;
                        lastReleaseTime = now;
                    }
                }
            }
        }
    }
    
    // If a click is pending and we exceed the double-click window, it's a single click!
    if (pendingSingleClick && (now - lastReleaseTime > DOUBLE_CLICK_WINDOW)) {
        pendingSingleClick = false;
        return GESTURE_SINGLE;
    }
    
    return GESTURE_NONE;
}

void skipMusicStream() {
    Serial.println("CMD:SKIP");
    Serial.println("[Music] Skipping to next Spotify track.");
    SpotifyCommand cmd = {SPOTIFY_CMD_NEXT, 0, false, ""};
    postSpotifyCommand(cmd);
    lastLCDUpdate = 0;
}

// ==========================================
// Quick Action Menu Helpers
// ==========================================
const unsigned long QUICK_ACTION_TIMEOUT_MS = 10000;
const char* quickActionLabels[] = {"Play / Pause", "Pick Playlist", "Shuffle", "Sleep Timer", "Timer", "White Noise", "Settings"};
const int QUICK_ACTION_COUNT = 7;
const int quickActionSleepMinutes[] = {0, 15, 30, 45};
const int QUICK_ACTION_SLEEP_COUNT = 4;
const int quickActionTimerMinutes[] = {2, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
const int QUICK_ACTION_TIMER_COUNT = 13;

String padQuickActionLine(const String& s) {
    String result = s;
    if (result.length() > 16) result = result.substring(0, 16);
    while (result.length() < 16) result += " ";
    return result;
}

void enterQuickActions() {
    currentState = STATE_QUICK_ACTIONS;
    quickActionMode = QUICK_ACTION_MAIN;
    quickActionItem = 0;
    quickActionSubItem = 0;
    quickActionPending = QUICK_PENDING_NONE;
    quickActionTimeout = millis() + QUICK_ACTION_TIMEOUT_MS;
    lastLCDUpdate = 0;
}

void exitQuickActions() {
    currentState = getSpotifyIsPlaying() ? STATE_MUSIC_MODE : STATE_IDLE_CLOCK;
    quickActionMode = QUICK_ACTION_MAIN;
    quickActionPending = QUICK_PENDING_NONE;
    lastLCDUpdate = 0;
}

void quickActionBackToMain() {
    quickActionMode = QUICK_ACTION_MAIN;
    quickActionPending = QUICK_PENDING_NONE;
    quickActionTimeout = millis() + QUICK_ACTION_TIMEOUT_MS;
    lastLCDUpdate = 0;
}

void renderQuickActions() {
    String l1, l2;
    switch (quickActionMode) {
        case QUICK_ACTION_MAIN:
            l1 = "  Quick Actions  ";
            if (quickActionItem == 2) {
                l2 = "> Shuffle: " + String(getSpotifyShuffleState() ? "On" : "Off");
            } else if (quickActionItem == 4) {
                if (isCountdownTimerActive()) {
                    TimerSnapshot ts = getTimerSnapshot();
                    l2 = "> Stop " + ts.timeString;
                } else {
                    l2 = "> Start Timer";
                }
            } else {
                l2 = "> " + String(quickActionLabels[quickActionItem]);
            }
            break;
        case QUICK_ACTION_DEVICE_SELECT:
            l1 = "  Select Device  ";
            if (getSpotifyNumDevices() == 0) {
                l2 = "> No devices";
            } else {
                l2 = "> " + getSpotifyDeviceName(quickActionSubItem);
            }
            break;
        case QUICK_ACTION_PLAYLIST_SELECT:
            l1 = "  Pick Playlist  ";
            if (getSpotifyNumPlaylists() == 0) {
                l2 = "> No playlists";
            } else {
                l2 = "> " + getSpotifyPlaylistName(quickActionSubItem);
            }
            break;
        case QUICK_ACTION_SLEEP_TIMER:
            l1 = "   Sleep Timer   ";
            {
                int mins = quickActionSleepMinutes[quickActionSubItem];
                l2 = (mins == 0) ? "> Off" : ("> " + String(mins) + " min");
            }
            break;
        case QUICK_ACTION_TIMER_SELECT:
            l1 = "   Set Timer    ";
            {
                int mins = quickActionTimerMinutes[quickActionSubItem];
                l2 = "> " + String(mins) + " min";
            }
            break;
    }
    displayLines(padQuickActionLine(l1), padQuickActionLine(l2));
}

void quickActionShowTemp(const String& line1, const String& line2) {
    displayLines(padQuickActionLine(line1), padQuickActionLine(line2));
}

void quickActionConfirmDevice() {
    if (getSpotifyNumDevices() == 0 || quickActionSubItem < 0 || quickActionSubItem >= getSpotifyNumDevices()) {
        exitQuickActions();
        return;
    }
    setSpotifySelectedDeviceIndex(quickActionSubItem);
    String deviceId = getSpotifyDeviceId(quickActionSubItem);
    SpotifyCommand cmd = {SPOTIFY_CMD_SELECT_DEVICE, 0, false, ""};
    strncpy(cmd.stringParam, deviceId.c_str(), sizeof(cmd.stringParam) - 1);
    postSpotifyCommand(cmd);
    if (quickActionPending == QUICK_PENDING_PLAY) {
        currentState = STATE_MUSIC_MODE;
        setSpotifyTrackText("Connecting...");
    } else if (quickActionPending == QUICK_PENDING_PLAYLIST) {
        String uri = getSpotifyPlaylistUri(getSpotifySelectedPlaylistIndex());
        SpotifyCommand playCmd = {SPOTIFY_CMD_PLAY_PLAYLIST, 0, false, ""};
        strncpy(playCmd.stringParam, uri.c_str(), sizeof(playCmd.stringParam) - 1);
        postSpotifyCommand(playCmd);
        currentState = STATE_MUSIC_MODE;
        setSpotifyTrackText("Connecting...");
    }
    quickActionPending = QUICK_PENDING_NONE;
    lastLCDUpdate = 0;
}

void quickActionPlaySelectedPlaylist() {
    if (getSpotifyNumPlaylists() == 0 || getSpotifySelectedPlaylistIndex() < 0 || getSpotifySelectedPlaylistIndex() >= getSpotifyNumPlaylists()) {
        exitQuickActions();
        return;
    }
    SpotifyCommand playCmd = {SPOTIFY_CMD_PLAY_PLAYLIST, 0, false, ""};
    String uri = getSpotifyPlaylistUri(getSpotifySelectedPlaylistIndex());
    strncpy(playCmd.stringParam, uri.c_str(), sizeof(playCmd.stringParam) - 1);
    postSpotifyCommand(playCmd);
    currentState = STATE_MUSIC_MODE;
    setSpotifyTrackText("Connecting...");
    lastLCDUpdate = 0;
}

void quickActionPerformPickPlaylist() {
    SpotifyCommand cmd = {SPOTIFY_CMD_FETCH_PLAYLISTS, 0, false, ""};
    postSpotifyCommand(cmd);
    quickActionMode = QUICK_ACTION_PLAYLIST_SELECT;
    setSpotifySelectedPlaylistIndex(0);
    quickActionSubItem = 0;
    quickActionPending = QUICK_PENDING_NONE;
    quickActionTimeout = millis() + QUICK_ACTION_TIMEOUT_MS;
    lastLCDUpdate = 0;
}

void quickActionPerformPlayPause() {
    bool wasPlaying = getSpotifyIsPlaying();
    SpotifyCommand cmd = {SPOTIFY_CMD_TOGGLE_PLAY_PAUSE, 0, false, ""};
    postSpotifyCommand(cmd);
    exitQuickActions();
    currentState = wasPlaying ? STATE_IDLE_CLOCK : STATE_MUSIC_MODE;
}

void quickActionPerformShuffle() {
    bool newState = !getSpotifyShuffleState();
    SpotifyCommand cmd = {SPOTIFY_CMD_SET_SHUFFLE, 0, newState, ""};
    postSpotifyCommand(cmd);
    quickActionShowTemp("   Shuffle    ", newState ? "On" : "Off");
    delay(1000);
    exitQuickActions();
}

void quickActionConfirmSleepTimer() {
    int mins = quickActionSleepMinutes[quickActionSubItem];
    sleepDurationMinutes = mins;
    if (mins > 0) {
        sleepTimerEnd = millis() + (mins * 60 * 1000L);
        if (hueLightsEnabled) {
            startHueSleepTimerFade(mins);
        }
        quickActionShowTemp("Sleep Timer", String(mins) + " min");
    } else {
        sleepTimerEnd = 0;
        quickActionShowTemp("Sleep Timer", "Off");
    }
    delay(1000);
    exitQuickActions();
}

void handleButtonGesture(ButtonGesture gesture) {
    if (gesture == GESTURE_NONE) return;
    Serial.printf("[Button] Gesture detected! Code = %d (1=Single, 2=Double, 3=Long)\r\n", (int)gesture);
    unsigned long now = millis();
    temporaryBacklightTimeout = now + 20000;

    // Offline setup & time setting take highest priority
    if (currentState == STATE_OFFLINE_PROMPT || currentState == STATE_DISCONNECTED) {
        Serial.println("[Offline] Entering manual time setup.");
        offlineSetupHour = 12;
        offlineSetupMin = 0;
        currentState = STATE_OFFLINE_SET_HOUR;
        lastLCDUpdate = 0;
        return;
    }
    else if (currentState == STATE_OFFLINE_SET_HOUR) {
        Serial.printf("[Offline] Hour set to %d. Setting minutes.\r\n", offlineSetupHour);
        currentState = STATE_OFFLINE_SET_MIN;
        lastLCDUpdate = 0;
        return;
    }
    else if (currentState == STATE_OFFLINE_SET_MIN) {
        Serial.printf("[Offline] Minute set to %d. Starting offline clock.\r\n", offlineSetupMin);
        struct tm mockTime;
        mockTime.tm_year = 2026 - 1900;
        mockTime.tm_mon = 7; // August
        mockTime.tm_mday = 20;
        mockTime.tm_hour = offlineSetupHour;
        mockTime.tm_min = offlineSetupMin;
        mockTime.tm_sec = 0;
        mockTime.tm_isdst = 0;
        time_t t = mktime(&mockTime);
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        
        currentState = STATE_IDLE_CLOCK;
        lastLCDUpdate = 0;
        return;
    }

    // If volume popup is active in idle clock state, a single click instantly confirms volume
    if (currentState == STATE_IDLE_CLOCK && volumeDisplayTimeout > 0 && gesture == GESTURE_SINGLE) {
        Serial.println("[Volume] Click confirmed volume level. Saving settings immediately.");
        volumeDisplayTimeout = 0;
        pendingSaveSettingsTime = 0;
        saveSettings();
        lastLCDUpdate = 0;
        return;
    }
    
    // A long press behavior depends on the active state
    if (gesture == GESTURE_LONG) {
        if (currentState == STATE_NOISE_MACHINE) {
            Serial.println("[Noise] Stop requested via long press. CMD:NOISE_OFF");
            stopAudioPlayback();
            currentState = STATE_IDLE_CLOCK;
            lastLCDUpdate = 0;
            return;
        } else if (currentState == STATE_MUSIC_MODE) {
            Serial.println("[Music] Entering Quick Actions from Music Mode via long press.");
            enterQuickActions();
            return;
        } else if (currentState == STATE_QUICK_ACTIONS) {
            Serial.println("[Quick Actions] Long press - exiting.");
            exitQuickActions();
            return;
        } else if (currentState != STATE_DISCONNECTED && currentState != STATE_SYNCING_TIME) {
            if (currentState == STATE_SETTINGS_MENU) {
                exitMenu(true);
            } else {
                enteredSettingsFromMusicMode = false;
                enterMenu();
            }
            return;
        }
    }
    
    // State-specific gesture handling
    if (currentState == STATE_SETTINGS_MENU) {
        // Reset menu timeout
        menuInactivityTimeout = now + 30000;
        
        if (gesture == GESTURE_SINGLE) {
            if (currentMenuState == MENU_NAVIGATING) {
                handleMenuSelect();
            } else { // MENU_EDITING
                // Save setting value and return to list navigation
                saveSettingValue(currentMenuItem);
                if (currentState == STATE_SETTINGS_MENU) {
                    currentMenuState = MENU_NAVIGATING;
                }
            }
            lastLCDUpdate = 0;
        }
    }
    else if (currentState == STATE_SUNRISE_CANCEL_PROMPT) {
        if (gesture == GESTURE_SINGLE || gesture == GESTURE_DOUBLE || gesture == GESTURE_LONG) {
            Serial.println("[Sunrise] User confirmed sunrise and upcoming alarm cancellation.");
            turnOffHueLight();
            sunriseActive = false;
            dismissUpcomingAlarmToday();
            currentState = STATE_IDLE_CLOCK;
            displayLines("Alarm Cancelled", "Lights Off");
            delay(1500);
            lastLCDUpdate = 0;
        }
    }
    else if (currentState == STATE_SNOOZE_CANCEL_PROMPT) {
        if (gesture == GESTURE_SINGLE || gesture == GESTURE_DOUBLE || gesture == GESTURE_LONG) {
            Serial.println("[Snooze] User confirmed snooze cancellation.");
            snoozeActive = false;
            currentState = STATE_IDLE_CLOCK;
            displayLines("Snooze Cancelled", "");
            delay(1500);
            lastLCDUpdate = 0;
        }
    }
    else if (currentState == STATE_ALERT_MESSAGE) {
        if (gesture == GESTURE_SINGLE || gesture == GESTURE_DOUBLE || gesture == GESTURE_LONG) {
            Serial.println("[Voice Memo] Opening replay/dismiss prompt.");
            stopAudioPlayback();
            cancelVoiceMemoPlayback();
            voiceMemoPromptItem = 0;
            voiceMemoPromptTimeout = now + 10000;
            currentState = STATE_VOICE_MEMO_PROMPT;
            lastLCDUpdate = 0;
        }
    }
    else if (currentState == STATE_VOICE_MEMO_PROMPT) {
        voiceMemoPromptTimeout = now + 10000;
        if (gesture == GESTURE_SINGLE) {
            if (voiceMemoPromptItem == 0) {
                Serial.println("[Voice Memo] User selected Replay.");
                stopAudioPlayback();
                startAudioPlayback("/voice_memo.wav");
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
            } else {
                Serial.println("[Voice Memo] User selected Dismiss.");
                stopAudioPlayback();
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
            }
        }
    }
    else if (currentState == STATE_NOISE_MACHINE) {
        if (gesture == GESTURE_SINGLE || gesture == GESTURE_DOUBLE || gesture == GESTURE_LONG) {
            Serial.println("[Noise] Stopped by user click. CMD:NOISE_OFF");
            stopAudioPlayback();
            currentState = STATE_IDLE_CLOCK;
            lastLCDUpdate = 0;
        }
    }
    else if (currentState == STATE_ALARM_RINGING) {
        if (gesture == GESTURE_SINGLE || gesture == GESTURE_DOUBLE || gesture == GESTURE_LONG) {
            if (snoozeDurationSelection > 0) {
                // Apply Snooze
                snoozeAlarm(snoozeDurationSelection);
            } else {
                // Dismiss Alarm
                dismissAlarm();
            }
        }
    }
    else if (currentState == STATE_MUSIC_MODE) {
        if (gesture == GESTURE_SINGLE) {
            // Play/Pause stream
            toggleMutePlay();
        }
        else if (gesture == GESTURE_DOUBLE) {
            // Skip song
            skipMusicStream();
        }
    }
    else if (currentState == STATE_IDLE_CLOCK) {
        if (sunriseActive) {
            Serial.println("[Sunrise] User clicked during active sunrise. Showing cancel prompt.");
            currentState = STATE_SUNRISE_CANCEL_PROMPT;
            sunriseCancelPromptTimeout = millis() + 5000;
            lastLCDUpdate = 0;
            return;
        }
        if (snoozeActive && gesture == GESTURE_SINGLE) {
            Serial.println("[Snooze] User clicked during active snooze. Showing cancel prompt.");
            currentState = STATE_SNOOZE_CANCEL_PROMPT;
            snoozeCancelPromptTimeout = millis() + 5000;
            lastLCDUpdate = 0;
            return;
        }
        if (gesture == GESTURE_SINGLE) {
            if (getSpotifyIsPlaying()) {
                Serial.println("[Clock] Single click while Spotify active. Toggling pause directly.");
                currentState = STATE_MUSIC_MODE;
                toggleMutePlay();
                return;
            }
            Serial.println("[Clock] Single click. Opening Quick Actions.");
            enterQuickActions();
        }
    }
    else if (currentState == STATE_QUICK_ACTIONS) {
        quickActionTimeout = now + QUICK_ACTION_TIMEOUT_MS;
        if (gesture == GESTURE_SINGLE) {
            if (quickActionMode == QUICK_ACTION_MAIN) {
                switch (quickActionItem) {
                    case 0: quickActionPerformPlayPause(); break;
                    case 1: quickActionPerformPickPlaylist(); break;
                    case 2: quickActionPerformShuffle(); break;
                    case 3:
                        quickActionMode = QUICK_ACTION_SLEEP_TIMER;
                        quickActionSubItem = 0;
                        lastLCDUpdate = 0;
                        break;
                    case 4:
                        if (isCountdownTimerActive()) {
                            stopCountdownTimer();
                            quickActionShowTemp("Timer", "Stopped");
                            delay(800);
                            exitQuickActions();
                        } else {
                            quickActionMode = QUICK_ACTION_TIMER_SELECT;
                            quickActionSubItem = 3; // Default to 15m
                            lastLCDUpdate = 0;
                        }
                        break;
                    case 5:
                        if (currentState == STATE_NOISE_MACHINE) {
                            Serial.println("[Quick Action] Stopping White Noise...");
                            stopAudioPlayback();
                            currentState = STATE_IDLE_CLOCK;
                            quickActionShowTemp("White Noise", "Stopped");
                            delay(800);
                            exitQuickActions();
                        } else {
                            Serial.println("[Quick Action] Starting White Noise...");
                            exitQuickActions();
                            currentState = STATE_NOISE_MACHINE;
                            startAudioPlayback("/brown_noise.wav", true);
                        }
                        break;
                    case 6:
                        exitQuickActions();
                        enteredSettingsFromMusicMode = false;
                        enterMenu();
                        break;
                }
            } else if (quickActionMode == QUICK_ACTION_DEVICE_SELECT) {
                quickActionConfirmDevice();
            } else if (quickActionMode == QUICK_ACTION_PLAYLIST_SELECT) {
                selectedSpotifyPlaylistIndex = quickActionSubItem;
                quickActionPlaySelectedPlaylist();
            } else if (quickActionMode == QUICK_ACTION_SLEEP_TIMER) {
                quickActionConfirmSleepTimer();
            } else if (quickActionMode == QUICK_ACTION_TIMER_SELECT) {
                int mins = quickActionTimerMinutes[quickActionSubItem];
                startCountdownTimer(mins);
                exitQuickActions();
                clockScreenIndex = 1; // Show Card 1 (Timer)
                screenCarouselTimeout = millis() + 10000;
                lastLCDUpdate = 0;
            }
        }
    }
}

void handleEncoderRotation(int delta) {
    Serial.printf("[Encoder] Rotary dial turned! delta = %d\r\n", delta);
    unsigned long now = millis();

    // If display is currently dark (Night Mode), first rotation wakes screen without taking action
    if (!isDisplayShouldBeOn()) {
        temporaryBacklightTimeout = now + 20000;
        updateHardwareBrightness();
        lastLCDUpdate = 0;
        Serial.println("[Display] Woken by dial rotation (no action taken).");
        return;
    }

    temporaryBacklightTimeout = now + 20000;
    
    if (currentState == STATE_OFFLINE_SET_HOUR) {
        offlineSetupHour += delta;
        if (offlineSetupHour < 0) offlineSetupHour = 23;
        if (offlineSetupHour > 23) offlineSetupHour = 0;
        lastLCDUpdate = 0;
        return;
    }
    if (currentState == STATE_OFFLINE_SET_MIN) {
        offlineSetupMin += delta;
        if (offlineSetupMin < 0) offlineSetupMin = 59;
        if (offlineSetupMin > 59) offlineSetupMin = 0;
        lastLCDUpdate = 0;
        return;
    }
    
    if (currentState == STATE_SETTINGS_MENU) {
        // Reset menu inactivity timeout
        menuInactivityTimeout = now + 30000;
        
        if (currentMenuState == MENU_NAVIGATING) {
            currentMenuItem += delta;
            int menuSize = getCurrentMenuSize();
            if (currentMenuItem < 0) currentMenuItem = menuSize - 1;
            if (currentMenuItem >= menuSize) currentMenuItem = 0;
            lastLCDUpdate = 0; // redraw menu list
        } 
        else if (currentMenuState == MENU_EDITING) {
            // Edit the selected menu item's value
            adjustSettingValue(currentMenuItem, delta);
            lastLCDUpdate = 0; // redraw menu editor
        }
    }
    else if (currentState == STATE_VOICE_MEMO_PROMPT) {
        voiceMemoPromptTimeout = now + 10000;
        voiceMemoPromptItem += delta;
        if (voiceMemoPromptItem < 0) voiceMemoPromptItem = 1;
        if (voiceMemoPromptItem > 1) voiceMemoPromptItem = 0;
        lastLCDUpdate = 0;
    }
    else if (currentState == STATE_QUICK_ACTIONS) {
        quickActionTimeout = now + QUICK_ACTION_TIMEOUT_MS;
        if (quickActionMode == QUICK_ACTION_MAIN) {
            quickActionItem += delta;
            if (quickActionItem < 0) quickActionItem = QUICK_ACTION_COUNT - 1;
            if (quickActionItem >= QUICK_ACTION_COUNT) quickActionItem = 0;
        } else if (quickActionMode == QUICK_ACTION_DEVICE_SELECT) {
            if (numSpotifyDevices > 0) {
                quickActionSubItem += delta;
                if (quickActionSubItem < 0) quickActionSubItem = numSpotifyDevices - 1;
                if (quickActionSubItem >= numSpotifyDevices) quickActionSubItem = 0;
            }
        } else if (quickActionMode == QUICK_ACTION_PLAYLIST_SELECT) {
            if (numSpotifyPlaylists > 0) {
                quickActionSubItem += delta;
                if (quickActionSubItem < 0) quickActionSubItem = numSpotifyPlaylists - 1;
                if (quickActionSubItem >= numSpotifyPlaylists) quickActionSubItem = 0;
            }
        } else if (quickActionMode == QUICK_ACTION_SLEEP_TIMER) {
            quickActionSubItem += delta;
            if (quickActionSubItem < 0) quickActionSubItem = QUICK_ACTION_SLEEP_COUNT - 1;
            if (quickActionSubItem >= QUICK_ACTION_SLEEP_COUNT) quickActionSubItem = 0;
        } else if (quickActionMode == QUICK_ACTION_TIMER_SELECT) {
            quickActionSubItem += delta;
            if (quickActionSubItem < 0) quickActionSubItem = QUICK_ACTION_TIMER_COUNT - 1;
            if (quickActionSubItem >= QUICK_ACTION_TIMER_COUNT) quickActionSubItem = 0;
        }
        lastLCDUpdate = 0;
    }
    else if (currentState == STATE_ALARM_RINGING) {
        // In active alarm, turning encoder cycles snooze durations!
        adjustSnoozeSelection(delta);
        lastLCDUpdate = 0;
    }
    else {
        // If in idle clock state, dial turns toggle date/weather display instead of adjusting volume
        if (currentState == STATE_IDLE_CLOCK) {
            if (sunriseActive) {
                Serial.println("[Sunrise] User rotated dial during active sunrise. Showing cancel prompt.");
                currentState = STATE_SUNRISE_CANCEL_PROMPT;
                sunriseCancelPromptTimeout = millis() + 5000;
                lastLCDUpdate = 0;
                return;
            }
            bool isTimerRunning = isCountdownTimerActive();
            bool canShowWeather = weatherEnabled && weatherFetched;
            bool canShowWmata = isWmataVisible();
            bool canShowGcal = isGCalEnabled();
            int numScreens = 1;
            if (isTimerRunning) numScreens++;
            if (canShowWeather) {
                numScreens += 3;
                if (canShowGcal) numScreens++;
                if (schoolCalendarEnabled) numScreens++;
                if (canShowWmata) numScreens++;
            }
            if (numScreens > 1) {
                if (delta > 0) { // Clockwise: Next Screen
                    clockScreenIndex = (clockScreenIndex + 1) % numScreens;
                } else if (delta < 0) { // Counter-clockwise: Previous Screen
                    clockScreenIndex = (clockScreenIndex + numScreens - 1) % numScreens;
                }
                screenCarouselTimeout = millis() + 20000; // 20-second timeout
                lastLCDUpdate = 0;
                
                int wmataScreenIdx = 4;
                if (isTimerRunning) wmataScreenIdx++;
                if (canShowGcal) wmataScreenIdx++;
                if (schoolCalendarEnabled) wmataScreenIdx++;
                if (clockScreenIndex == wmataScreenIdx && canShowWmata && !wmataDataValid) {
                    invalidateWmataCache();
                }
                Serial.printf("[Clock] Screen changed to %d via dial (Total screens: %d).\r\n", clockScreenIndex, numScreens);
            }
            return;
        }

        // Default behavior: Adjust volume
        if (currentState == STATE_MUSIC_MODE || (currentState == STATE_IDLE_CLOCK && getSpotifyIsPlaying())) {
            if (currentState == STATE_IDLE_CLOCK) {
                currentState = STATE_MUSIC_MODE;
            }
            // Spotify remote volume (independent from local amp)
            int newVol = getSpotifyVolumePercent() + delta * 5;
            if (newVol < 0) newVol = 0;
            if (newVol > 100) newVol = 100;
            setSpotifyVolumePercent(newVol);
            setPendingSpotifyVolume(newVol);
            volumeDisplayTimeout = now + 2000;
            lastLCDUpdate = 0;
            Serial.printf("CMD:SPOTIFY_VOL=%d\r\n", newVol);
        } else {
            // Local amplifier / alarm / noise volume
            int newVolume = currentVolume + delta;
            if (newVolume < 0) newVolume = 0;
            if (newVolume > 21) newVolume = 21;
            
            if (newVolume != currentVolume) {
                currentVolume = newVolume;
                isMuted = false;
                updateAudioVolume();
                pendingSaveSettingsTime = now + 2000; // Debounce NVS flash saving
                volumeDisplayTimeout = now + 2000;
                lastLCDUpdate = 0;
                
                // Output serial command for external companion control
                Serial.printf("CMD:VOL=%d\r\n", currentVolume);
            }
        }
    }
}

volatile bool isOtaUpdating = false;

void setupOTA() {
    // Port defaults to 3232
    ArduinoOTA.setPort(3232);

    // Hostname defaults to esp3232-[MAC]
    ArduinoOTA.setHostname("spotify-alarm");

    ArduinoOTA.onStart([]() {
        isOtaUpdating = true;
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else { // U_FS
            type = "filesystem";
        }
        Serial.println("Start updating " + type);
        displayLines("OTA Update...", "Start...");
    });
    
    ArduinoOTA.onEnd([]() {
        isOtaUpdating = false;
        Serial.println("\nEnd");
        displayLines("OTA Complete!", "Rebooting...");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static int lastPct = -1;
        int pct = (progress / (total / 100));
        if (pct >= lastPct + 10 || pct == 100) {
            lastPct = pct;
            char buf[17];
            snprintf(buf, sizeof(buf), "Progress: %d%%", pct);
            displayLines("OTA Update...", String(buf));
        }
        Serial.printf("Progress: %u%%\r", pct);
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        isOtaUpdating = false;
        Serial.printf("Error[%u]: ", error);
        String errStr = "Error";
        if (error == OTA_AUTH_ERROR) errStr = "Auth Failed";
        else if (error == OTA_BEGIN_ERROR) errStr = "Begin Failed";
        else if (error == OTA_CONNECT_ERROR) errStr = "Connect Failed";
        else if (error == OTA_RECEIVE_ERROR) errStr = "Receive Failed";
        else if (error == OTA_END_ERROR) errStr = "End Failed";
        
        displayLines("OTA Failed!", errStr);
        delay(2000);
        lastLCDUpdate = 0; // Trigger immediate LCD redraw back to current state
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] ArduinoOTA service initialized.");
}

void createLocalWav(const char* path, uint32_t frequency, uint32_t patternType) {
    Serial.printf("[SPIFFS] Generating local file: %s...\r\n", path);
    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("[SPIFFS] Error creating file: %s\r\n", path);
        return;
    }
    
    uint32_t sampleRate = 22050;
    uint32_t durationSeconds = 2;
    uint32_t numSamples = sampleRate * durationSeconds;
    uint32_t dataSize = numSamples * 2; // 16-bit mono
    uint32_t fileSize = dataSize + 36;
    
    // Write WAV Header
    file.write((const uint8_t*)"RIFF", 4);
    file.write((const uint8_t*)&fileSize, 4);
    file.write((const uint8_t*)"WAVE", 4);
    file.write((const uint8_t*)"fmt ", 4);
    uint32_t subchunk1Size = 16;
    file.write((const uint8_t*)&subchunk1Size, 4);
    uint16_t audioFormat = 1; // PCM
    file.write((const uint8_t*)&audioFormat, 2);
    uint16_t numChannels = 1; // Mono
    file.write((const uint8_t*)&numChannels, 2);
    file.write((const uint8_t*)&sampleRate, 4);
    uint32_t byteRate = sampleRate * 2;
    file.write((const uint8_t*)&byteRate, 4);
    uint16_t blockAlign = 2;
    file.write((const uint8_t*)&blockAlign, 2);
    uint16_t bitsPerSample = 16;
    file.write((const uint8_t*)&bitsPerSample, 2);
    file.write((const uint8_t*)"data", 4);
    file.write((const uint8_t*)&dataSize, 4);
    
    float brownState = 0.0f;
    for (uint32_t i = 0; i < numSamples; i++) {
        float t = (float)i / sampleRate;
        float val_f = 0.0f;
        
        if (patternType == 1) {
            float strikeTimes[2] = {0.0f, 1.0f};
            for (int s = 0; s < 2; s++) {
                if (t >= strikeTimes[s]) {
                    float dt = t - strikeTimes[s];
                    float env = exp(-3.5f * dt);
                    float wave = (
                        1.0f * sin(2.0f * PI * 587.33f * dt) +
                        0.5f * sin(2.0f * PI * 1174.66f * dt) +
                        0.2f * sin(2.0f * PI * 1762.00f * dt)
                    ) / 1.7f;
                    val_f += wave * env;
                }
            }
        }
        else if (patternType == 2) {
            float env = exp(-1.8f * t);
            float wave = (
                1.0f * sin(2.0f * PI * 523.25f * t) +
                0.8f * sin(2.0f * PI * 659.25f * t) +
                0.7f * sin(2.0f * PI * 783.99f * t) +
                0.5f * sin(2.0f * PI * 1046.50f * t)
            ) / 3.0f;
            val_f = wave * env;
        }
        else if (patternType == 3) {
            float env = exp(-1.2f * t);
            float tremolo = 0.85f + 0.15f * sin(2.0f * PI * 4.0f * t);
            float wave = (
                1.0f * sin(2.0f * PI * 329.63f * t) +
                0.8f * sin(2.0f * PI * 332.63f * t) +
                0.3f * sin(2.0f * PI * 659.25f * t)
            ) / 2.1f;
            val_f = wave * env * tremolo;
        }
        else if (patternType == 4) {
            float noteStarts[4] = {0.0f, 0.18f, 0.36f, 0.54f};
            float noteFreqs[4] = {523.25f, 659.25f, 783.99f, 1046.50f};
            
            for (int n = 0; n < 4; n++) {
                if (t >= noteStarts[n]) {
                    float dt = t - noteStarts[n];
                    float env = exp(-6.0f * dt);
                    val_f += 0.45f * sin(2.0f * PI * noteFreqs[n] * dt) * env;
                }
            }
            float t2 = t - 1.0f;
            for (int n = 0; n < 4; n++) {
                if (t2 >= noteStarts[n]) {
                    float dt = t2 - noteStarts[n];
                    float env = exp(-6.0f * dt);
                    val_f += 0.45f * sin(2.0f * PI * noteFreqs[n] * dt) * env;
                }
            }
        }
        else if (patternType == 5) {
            float pulsePeriod = 0.4f;
            float pulseTime = fmod(t, pulsePeriod);
            if (pulseTime < 0.2f) {
                float fade = 1.0f;
                if (pulseTime < 0.02f) {
                    fade = pulseTime / 0.02f;
                } else if (pulseTime > 0.18f) {
                    fade = (0.2f - pulseTime) / 0.02f;
                }
                val_f = 0.8f * sin(2.0f * PI * 900.0f * t) * fade;
            }
        }
        else if (patternType == 6) {
            float r = (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
            brownState = 0.99f * brownState + 0.015f * r;
            val_f = brownState * 7.5f;
        }
        
        if (val_f > 1.0f) val_f = 1.0f;
        if (val_f < -1.0f) val_f = -1.0f;
        
        int16_t sample16 = (int16_t)(val_f * 32760.0f);
        file.write((uint8_t)(sample16 & 0xFF));
        file.write((uint8_t)((sample16 >> 8) & 0xFF));
    }
    
    file.close();
    Serial.printf("[SPIFFS] %s created successfully!\r\n", path);
}

void setupSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Failed to mount SPIFFS.");
        return;
    }
    
    File testFile = SPIFFS.open("/buzzer.wav", FILE_READ);
    bool overwrite = true;
    if (testFile) {
        if (testFile.size() == 88244) {
            overwrite = false;
        }
        testFile.close();
    }
    
    if (!SPIFFS.exists("/white_noise_v3.wav")) {
        overwrite = true;
    }

    // Regenerate if the expected generated files are missing
    if (!SPIFFS.exists("/white_noise.wav") || !SPIFFS.exists("/buzzer.wav")) {
        overwrite = true;
    }

    if (overwrite) {
        Serial.println("[SPIFFS] Generating local audio files...");
        SPIFFS.remove("/buzzer.wav");
        SPIFFS.remove("/chime.wav");
        SPIFFS.remove("/fast_alert.wav");
        SPIFFS.remove("/siren.wav");
        SPIFFS.remove("/chirp.wav");
        SPIFFS.remove("/white_noise.wav");
        SPIFFS.remove("/white_noise_v2.wav");
        SPIFFS.remove("/white_noise_v3.wav");

        createLocalWav("/buzzer.wav", 0, 1);
        createLocalWav("/chime.wav", 0, 2);
        createLocalWav("/fast_alert.wav", 0, 3);
        createLocalWav("/siren.wav", 0, 4);
        createLocalWav("/chirp.wav", 0, 5);
        createLocalWav("/white_noise.wav", 0, 6);

        File v3 = SPIFFS.open("/white_noise_v3.wav", FILE_WRITE);
        if (v3) v3.close();
        
        Serial.println("[SPIFFS] Local audio files generated.");
    } else {
        Serial.println("[SPIFFS] Local audio files verified.");
    }
}

bool isTimeSynchronized() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        bool synced = (timeinfo.tm_year > 70);
        if (synced) {
            Serial.printf("[NTP] Time check: %04d-%02d-%02d %02d:%02d:%02d (year=%d)\r\n", 
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, timeinfo.tm_year);
        }
        return synced;
    }
    return false;
}

void spotifyTask(void* pvParameters) {
    SpotifyCommand cmd;
    static unsigned long lastSpotifyPoll = 0;
    for (;;) {
        unsigned long now = millis();
        while (receiveSpotifyCommand(cmd, 0)) {
            processSpotifyCommand(cmd);
        }
        sendPendingSpotifyVolume();
        if (currentState == STATE_MUSIC_MODE && (now - lastSpotifyPoll) >= 3500) {
            lastSpotifyPoll = now;
            pollSpotifyPlaybackState();
        }
        continuePendingSpotifyPlayback();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void networkPollTask(void* pvParameters) {
    for (;;) {
        if (isOtaUpdating) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        size_t freeHeap = ESP.getFreeHeap();
        bool isAudioPlaying = (currentState == STATE_ALARM_RINGING || currentState == STATE_MUSIC_MODE || audio.isRunning());
        
        // Only run heavy SSL network syncs if free heap is safe (>35KB) and audio is not actively streaming
        if (freeHeap >= 35000 && !isAudioPlaying && WiFi.status() == WL_CONNECTED) {
            unsigned long now = millis();
            updateWeather();
            vTaskDelay(pdMS_TO_TICKS(300));
            updateSchoolCalendar();
            vTaskDelay(pdMS_TO_TICKS(300));
            updateGCal();
            vTaskDelay(pdMS_TO_TICKS(300));
            updateWmataBusPredictions();
            vTaskDelay(pdMS_TO_TICKS(300));

            bool inHueMenu = (currentState == STATE_SETTINGS_MENU && currentMenuLevel == LEVEL_HUE_LIGHTS);
            bool webClientActive = (now - lastWebActivityTime < 30000);
            if ((inHueMenu || webClientActive) && (now - lastHueStatePoll >= HUE_STATE_POLL_INTERVAL)) {
                lastHueStatePoll = now;
                fetchHueLightState();
            }
        }

        // Night Mode USB Power Bank Keep-Alive:
        // When in Night Mode with the screen dark and no music/audio playing, current draw drops to ~20mA,
        // causing USB power banks (like the Anker A1653) to enter rapid-blinking shutdown and cut power after 30s.
        // Disabling Wi-Fi modem sleep provides a rock-steady, flat ~85mA load without any pulsing or blinking.
        static bool s_wifiSleepDisabled = false;
        bool isNightAndScreenDark = isNightMode && !isDisplayShouldBeOn();
        bool shouldDisableSleep = isNightAndScreenDark && !isAudioPlaying && (WiFi.status() == WL_CONNECTED);

        if (shouldDisableSleep != s_wifiSleepDisabled) {
            s_wifiSleepDisabled = shouldDisableSleep;
            WiFi.setSleep(!s_wifiSleepDisabled);
            Serial.printf("[Power] Wi-Fi Modem Sleep %s (steady power bank load: %s)\r\n", 
                          s_wifiSleepDisabled ? "DISABLED" : "ENABLED", 
                          s_wifiSleepDisabled ? "ACTIVE (~85mA)" : "OFF (~20mA)");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void handleSerialCommands() {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        
        if (input == "alarm on") {
            alarmEnabled = true;
            saveSettings();
            Serial.println("[Serial] Alarm enabled.");
            lastLCDUpdate = 0;
        } else if (input == "alarm off") {
            alarmEnabled = false;
            saveSettings();
            Serial.println("[Serial] Alarm disabled.");
            lastLCDUpdate = 0;
        } else if (input == "alarm trigger" || input == "alarm ring") {
            Serial.println("[Serial] Force-triggering alarm!");
            startAlarmPlayback();
            currentState = STATE_ALARM_RINGING;
            lastLCDUpdate = 0;
        } else if (input == "alarm dismiss") {
            Serial.println("[Serial] Force-dismissing alarm!");
            dismissAlarm();
        } else if (input == "weather alert" || input == "weather alert on") {
            weatherAlertActive = true;
            weatherAlertTitle = "Svr T-Stm Warning";
            Serial.println("[Serial] Test Weather Alert ACTIVATED: Svr T-Stm Warning");
            lastLCDUpdate = 0;
        } else if (input == "weather alert off") {
            weatherAlertActive = false;
            weatherAlertTitle = "";
            Serial.println("[Serial] Test Weather Alert DEACTIVATED.");
            lastLCDUpdate = 0;
        } else if (input.startsWith("weather alert ")) {
            weatherAlertActive = true;
            weatherAlertTitle = input.substring(14);
            Serial.printf("[Serial] Custom Weather Alert ACTIVATED: %s\r\n", weatherAlertTitle.c_str());
            lastLCDUpdate = 0;
        } else if (input.startsWith("alarm ")) {
            int spaceIndex = input.indexOf(' ', 6);
            if (spaceIndex > 0) {
                int h = input.substring(6, spaceIndex).toInt();
                int m = input.substring(spaceIndex + 1).toInt();
                if (h >= 0 && h < 24 && m >= 0 && m < 60) {
                    alarmHour = h;
                    alarmMinute = m;
                    alarmEnabled = true;
                    saveSettings();
                    Serial.printf("[Serial] Alarm set to %02d:%02d\r\n", alarmHour, alarmMinute);
                    lastLCDUpdate = 0;
                } else {
                    Serial.println("[Serial] Invalid alarm values. Use HH (0-23) MM (0-59).");
                }
            }
        } else if (input.startsWith("play ")) {
            String url = input.substring(5);
            url.trim();
            if (url.length() > 0) {
                startAudioPlayback(url.c_str());
            }
        } else if (input == "stop") {
            stopAudioPlayback();
        } else if (input.startsWith("volume ")) {
            int vol = input.substring(7).toInt();
            if (vol >= 0 && vol <= 21) {
                currentVolume = vol;
                isMuted = false;
                updateAudioVolume();
                saveSettings();
                volumeDisplayTimeout = millis() + 2000;
                lastLCDUpdate = 0;
            } else {
                Serial.println("[Serial] Volume range is 0 to 21.");
            }
        } else if (input.startsWith("msg:")) {
            int firstColon = 3;
            int secondColon = input.indexOf(':', firstColon + 1);
            if (secondColon > 0) {
                String sound = input.substring(firstColon + 1, secondColon);
                String msgText = input.substring(secondColon + 1);
                msgText.trim();
                if (msgText.length() > 0) {
                    intercomMessage = msgText;
                    currentState = STATE_ALERT_MESSAGE;
                    stopAudioPlayback();
                    
                    if (sound == "chime") startAudioPlayback("/chime.wav");
                    else if (sound == "chirp") startAudioPlayback("/chirp.wav");
                    else if (sound == "siren") startAudioPlayback("/siren.wav");
                    else if (sound == "buzzer") startAudioPlayback("/buzzer.wav");
                    else if (sound == "fast_alert") startAudioPlayback("/fast_alert.wav");
                    
                }
            } else {
                Serial.println("[Serial] Invalid format. Use msg:<sound>:<text> (e.g. msg:chime:Hello!)");
            }
        } else if (input == "click" || input == "press" || input == "setup") {
            Serial.println("[Serial] Simulated Button Click!");
            handleButtonGesture(GESTURE_SINGLE);
        } else if (input == "longpress" || input == "hold") {
            Serial.println("[Serial] Simulated Long Press!");
            handleButtonGesture(GESTURE_LONG);
        } else if (input == "rotate right" || input == "right" || input == "+") {
            Serial.println("[Serial] Simulated Rotate +1");
            handleEncoderRotation(1);
        } else if (input == "rotate left" || input == "left" || input == "-") {
            Serial.println("[Serial] Simulated Rotate -1");
            handleEncoderRotation(-1);
        } else if (input == "status") {
            struct tm timeinfo;
            char timeBuf[64] = "Not Sync'd";
            if (getLocalTime(&timeinfo, 0)) {
                strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            }
            Serial.println("\n--- ALARM CLOCK STATUS ---");
            Serial.printf("Local Time:    %s\r\n", timeBuf);
            Serial.printf("State:         %d\r\n", currentState);
            Serial.printf("WiFi Status:   %s\r\n", (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED");
            Serial.printf("Alarm Setup:   %02d:%02d (%s)\r\n", alarmHour, alarmMinute, alarmEnabled ? "ENABLED" : "DISABLED");
            Serial.printf("Current Vol:   %d (Muted: %s)\r\n", currentVolume, isMuted ? "YES" : "NO");
            Serial.printf("Stream Status: %s\r\n", audio.isRunning() ? "PLAYING" : "STOPPED");
            Serial.println("--------------------------\n");
        } else {
            Serial.println("[Serial] Unknown command. Supported commands:");
            Serial.println(" - alarm <hour> <minute> : set alarm time (e.g. alarm 7 30)");
            Serial.println(" - alarm on / alarm off  : toggle alarm state");
            Serial.println(" - alarm trigger         : instantly force alarm to ringing state");
            Serial.println(" - alarm dismiss         : force dismiss alarm and show affirmation");
            Serial.println(" - play <url>            : play manual MP3 audio stream");
            Serial.println(" - stop                  : stop current stream");
            Serial.println(" - volume <0-21>         : set system volume level");
            Serial.println(" - msg:<sound>:<text>    : send intercom alert (e.g. msg:chime:Wake up!)");
            Serial.println(" - status                : print system parameters");
        }
    }
}

void pollSeesawEncoder() {
#ifndef SIMULATION_MODE
    int delta = 0;
    int32_t currentPos = ss.getEncoderPosition();
    int32_t diff = currentPos - lastEncoderPosition;
    
    if (diff != 0) {
        delta = diff;
        lastEncoderPosition = currentPos;
    }

    if (delta != 0) {
        handleEncoderRotation(delta);
    }
    
    ButtonGesture gesture = checkButtonGestures();
    if (gesture != GESTURE_NONE) {
        handleButtonGesture(gesture);
    }
#endif
}

#ifdef SIMULATION_MODE
void pollSimulationEncoder() {
    int delta = 0;
    int clkState = digitalRead(SIM_ENCODER_CLK);
    static int lastClkState = HIGH;
    
    if (clkState != lastClkState) {
        if (clkState == LOW) {
            if (digitalRead(SIM_ENCODER_DT) != clkState) {
                delta = 1;
            } else {
                delta = -1;
            }
        }
        lastClkState = clkState;
    }

    if (delta != 0) {
        handleEncoderRotation(delta);
    }
    
    ButtonGesture gesture = checkButtonGestures();
    if (gesture != GESTURE_NONE) {
        handleButtonGesture(gesture);
    }
}
#endif

bool shouldAlarmTrigger(struct tm &timeinfo) {
    if (!alarmEnabled) return false;
    if (timeinfo.tm_hour != alarmHour || timeinfo.tm_min != alarmMinute) {
        alarmDismissedForToday = false; // Reset volatile flag when not in alarm minute
        return false;
    }
    
    // Calendar-day NVS latch: If alarm was already dismissed today, never fire again!
    if (timeinfo.tm_yday == lastAlarmDismissedDay) {
        return false;
    }
    
    if (alarmDismissedForToday) {
        return false;
    }
    
    if (alarmRepeatMode == REPEAT_EVERYDAY) {
        return true;
    } else if (alarmRepeatMode == REPEAT_WEEKDAYS) {
        // Mon-Fri (1 to 5)
        return (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5);
    } else if (alarmRepeatMode == REPEAT_WEEKENDS) {
        // Sat-Sun (6 or 0)
        return (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6);
    } else if (alarmRepeatMode == REPEAT_SCHOOL_SCHEDULE) {
        // Alarm only rings on school days (weekdays not in the days-off calendar)
        return isSchoolDay(&timeinfo);
    } else if (alarmRepeatMode == REPEAT_ONCE) {
        return true;
    }
    return false;
}

void manageSystemStates() {
    unsigned long now = millis();
    struct tm timeinfo;
    bool timeAvailable = getLocalTime(&timeinfo, 0);
    if (timeAvailable) {
        updateNightModeState(timeinfo.tm_hour, timeinfo.tm_min);
    }
    
    // Check and trigger pre-alarm Hue sunrise transition before alarm
    static bool sunriseTriggeredThisMinute = false;
    if (timeAvailable && hueSunriseEnabled && alarmEnabled) {
        int dur = (hueSunriseDuration > 0) ? hueSunriseDuration : 20;
        int sunriseHour = alarmHour;
        int sunriseMin = alarmMinute - dur;
        if (sunriseMin < 0) {
            sunriseMin += 60;
            sunriseHour -= 1;
            if (sunriseHour < 0) {
                sunriseHour += 24;
            }
        }
        
        if (timeinfo.tm_hour == sunriseHour && timeinfo.tm_min == sunriseMin) {
            if (!sunriseTriggeredThisMinute) {
                // Determine target day of week for repeat mode checks,
                // accounting for midnight crossing backwards.
                int targetWday = timeinfo.tm_wday;
                if (alarmMinute - dur < 0) {
                    targetWday = (targetWday + 1) % 7;
                }
                
                bool dayMatches = false;
                if (alarmRepeatMode == REPEAT_EVERYDAY) dayMatches = true;
                else if (alarmRepeatMode == REPEAT_WEEKDAYS) {
                    dayMatches = (targetWday >= 1 && targetWday <= 5);
                } else if (alarmRepeatMode == REPEAT_WEEKENDS) {
                    dayMatches = (targetWday == 0 || targetWday == 6);
                } else if (alarmRepeatMode == REPEAT_SCHOOL_SCHEDULE) {
                    struct tm targetTm = timeinfo;
                    if (alarmMinute - dur < 0) {
                        time_t targetTime = mktime(&targetTm) + 86400;
                        localtime_r(&targetTime, &targetTm);
                    }
                    dayMatches = isSchoolDay(&targetTm);
                } else if (alarmRepeatMode == REPEAT_ONCE) {
                    dayMatches = true;
                }
                
                if (dayMatches) {
                    Serial.printf("[Hue] Triggering %d-min early sunrise (alarm at %02d:%02d)\r\n", dur, alarmHour, alarmMinute);
                    triggerHueSunriseTransitionAsync();
                    sunriseActive = true;
                }
                sunriseTriggeredThisMinute = true;
            }
        } else {
            sunriseTriggeredThisMinute = false;
        }
    } else {
        sunriseTriggeredThisMinute = false;
    }
    
    // Update LCD hardware brightness dynamically
    updateHardwareBrightness();
    
    // Auto-revert secondary screen display to Date screen 0 after 20 seconds of inactivity
    if (currentState == STATE_IDLE_CLOCK && clockScreenIndex != 0 && screenCarouselTimeout > 0 && now >= screenCarouselTimeout) {
        clockScreenIndex = 0;
        screenCarouselTimeout = 0;
        lastLCDUpdate = 0;
        Serial.println("[Clock] Screen timed out, reverted to Date screen 0.");
    }
    
    // Auto-revert to clock face after 1 minute of paused/inactive music
    if (currentState == STATE_MUSIC_MODE) {
        if (getSpotifyIsPlaying()) {
            spotifyPausedStartTime = 0;
        } else {
            if (spotifyPausedStartTime == 0) {
                spotifyPausedStartTime = now;
            } else if (now - spotifyPausedStartTime >= 60000) { // 1-minute timeout
                Serial.println("[Music] Paused/inactive timeout. Reverting to Clock Face.");
                currentState = STATE_IDLE_CLOCK;
                spotifyPausedStartTime = 0;
                lastLCDUpdate = 0;
            }
        }
    } else {
        spotifyPausedStartTime = 0;
    }
    
    static bool showingVolume = false;
    bool shouldShowVolume = (now < volumeDisplayTimeout);
    if (showingVolume != shouldShowVolume) {
        showingVolume = shouldShowVolume;
        lastLCDUpdate = 0;
    }
    
    if (currentState == STATE_SETTINGS_MENU && now >= menuInactivityTimeout) {
        Serial.println("[Menu] Inactivity timeout. Exiting menu.");
        exitMenu(true);
    }

    if (currentState == STATE_QUICK_ACTIONS && now >= quickActionTimeout) {
        Serial.println("[Quick Actions] Inactivity timeout. Exiting.");
        exitQuickActions();
    }

    // Auto-dismiss Alarm after 15 minutes max continuous ringing safety cap
    static unsigned long alarmStartRingingTime = 0;
    if (currentState == STATE_ALARM_RINGING) {
        if (alarmStartRingingTime == 0) alarmStartRingingTime = now;
        else if (now - alarmStartRingingTime >= 900000) { // 15 minutes
            Serial.println("[Alarm] Max 15-min ringing timeout reached. Auto-dismissing alarm.");
            dismissAlarm();
            alarmStartRingingTime = 0;
        }
    } else {
        alarmStartRingingTime = 0;
    }

    // Daily 3:00 AM Atomic NTP time re-sync
    static bool dailyNtpSyncedToday = false;
    if (timeAvailable) {
        if (timeinfo.tm_hour == 3 && timeinfo.tm_min == 0) {
            if (!dailyNtpSyncedToday && WiFi.status() == WL_CONNECTED) {
                Serial.println("[NTP] Running daily 3:00 AM atomic time re-sync...");
                configTzTime(DEFAULT_TIMEZONE, NTP_SERVER);
                dailyNtpSyncedToday = true;
            }
        } else {
            dailyNtpSyncedToday = false;
        }
    }
    
    switch (currentState) {
        case STATE_DISCONNECTED: {
            displayLines("Please connect", "to WiFi...");
            
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("\n[WiFi] Connected. IP: %s\r\n", WiFi.localIP().toString().c_str());
                
                setHeartCustomChar();
                displayLines("  Hi Faye! \x07   ", "Ready for Today!");
                delay(3000); // Display greeting for 3 seconds
                
                static bool s_webAndOtaInitialized = false;
                if (!s_webAndOtaInitialized) {
                    s_webAndOtaInitialized = true;
                    setupWebServer();
                    setupOTA();
                }
                
                currentState = STATE_SYNCING_TIME;
                ntpStart = millis();
                
                configTzTime(DEFAULT_TIMEZONE, NTP_SERVER);
                Serial.println("[NTP] NTP time sync initialized.");
            } else if (now - disconnectedStartTime > 30000) {
                Serial.println("[WiFi] Connection timed out. Prompting for Offline Mode.");
                currentState = STATE_OFFLINE_PROMPT;
                lastLCDUpdate = 0;
            }
            break;
        }
        
        case STATE_SYNCING_TIME: {
            displayLines("Syncing Time...", "Please wait...");
            
            if (WiFi.status() != WL_CONNECTED) {
                currentState = STATE_DISCONNECTED;
            } else if (isTimeSynchronized()) {
                Serial.println("[NTP] Clock successfully synchronized.");
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
            } else if (now - ntpStart > 6000) {
                Serial.println("[NTP] Sync timeout! Setting mock fallback time.");
                
#ifdef SIMULATION_MODE
                // Wokwi simulation: use current UTC time and apply timezone
                time_t utc_now = time(nullptr);
                struct timeval tv = { .tv_sec = utc_now, .tv_usec = 0 };
                settimeofday(&tv, nullptr);
                Serial.println("[NTP] Using current UTC time for Wokwi simulation.");
#else
                struct tm mockTime;
                mockTime.tm_year = 2024 - 1900;
                mockTime.tm_mon = 0;
                mockTime.tm_mday = 1;
                mockTime.tm_hour = 12;
                mockTime.tm_min = 0;
                mockTime.tm_sec = 0;
                mockTime.tm_isdst = 0;
                
                time_t t = mktime(&mockTime);
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                settimeofday(&tv, nullptr);
#endif
                
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
            }
            break;
        }
        
        case STATE_IDLE_CLOCK: {
            
            if (timeAvailable) {
                bool triggerAlarm = false;
                if (snoozeActive) {
                    if (timeinfo.tm_hour == snoozeHour && timeinfo.tm_min == snoozeMinute) {
                        triggerAlarm = true;
                    }
                } else if (shouldAlarmTrigger(timeinfo)) {
                    triggerAlarm = true;
                }
                
                if (triggerAlarm) {
                    if (!alarmTriggeredThisMinute) {
                        Serial.println("[Alarm] Alarm triggered!");
                        alarmTriggeredThisMinute = true;
                        snoozeActive = false;
                        sunriseActive = false; // reset sunrise active flag
                        currentState = STATE_ALARM_RINGING;
                        startAlarmPlayback();
                        break;
                    }
                } else {
                    alarmTriggeredThisMinute = false;
                }
                
                // Scheduled reminder check
                if (reminderEnabled && !reminderText.isEmpty()) {
                    if (timeinfo.tm_hour == reminderHour && timeinfo.tm_min == reminderMinute) {
                        if (!reminderTriggeredThisMinute) {
                            Serial.println("[Reminder] Scheduled reminder triggered!");
                            reminderTriggeredThisMinute = true;
                            intercomMessage = reminderText;
                            alertHeader = "   REMINDER!    ";
                            currentState = STATE_ALERT_MESSAGE;
                            // Auto-disable so it only fires once
                            reminderEnabled = false;
                            saveSettings();
                            // Play selected sound
                            stopAudioPlayback();
                            playNamedSound(reminderSound);
                            lastLCDUpdate = 0;
                            break;
                        }
                    } else {
                        reminderTriggeredThisMinute = false;
                    }
                }
            }
            
            if (now - lastLCDUpdate >= LCD_UPDATE_INTERVAL) {
                lastLCDUpdate = now;
                if (timeAvailable) {
                    WeatherSnapshot w = getWeatherSnapshot();
                    bool weatherEnabled = w.enabled;
                    bool weatherFetched = w.fetched;
                    int weatherTemp = w.temp;
                    const String& weatherLabel = w.label;
                    int weatherFeelsLike = w.feelsLike;
                    float weatherPrecipitationHours = w.precipitationHours;
                    int weatherRainStartHour = w.rainStartHour;
                    int weatherPrecipitationChance = w.precipitationChance;
                    int weatherHighTemp = w.highTemp;
                    int weatherUVIndex = w.uvIndex;
                    int weatherCodeDaily = w.codeDaily;
                    bool weatherAlertActive = w.alertActive;
                    const String& weatherAlertTitle = w.alertTitle;

                    bool isNightBigClock = (nightBigClock == 1);
                    if (nightBigClock == 2) {
                        int nowMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
                        int wakeMins = (alarmEnabled) ? (alarmHour * 60 + alarmMinute) : (7 * 60);
                        if (wakeMins < 22 * 60) {
                            isNightBigClock = (nowMins >= 22 * 60 || nowMins < wakeMins);
                        } else {
                            isNightBigClock = (nowMins >= 22 * 60 && nowMins < wakeMins);
                        }
                    }
                    if (clockScreenIndex == 0 && isNightBigClock) {
                        drawBigClock(timeinfo.tm_hour, timeinfo.tm_min);
                        lastCreatedCharScreenIndex = -1; // Invalidate so returning to any screen reloads dot glyphs
                        break;
                    }

                    bool wmataVisible = isWmataVisible();
                    bool wmataHandled = false;
                    int displayHour = timeinfo.tm_hour;
                    const char* ampm = "AM";
                    if (displayHour >= 12) {
                        ampm = "PM";
                    }
                    if (displayHour > 12) {
                        displayHour -= 12;
                    }
                    if (displayHour == 0) {
                        displayHour = 12;
                    }
                    
                    char timeStr[17];
                    snprintf(timeStr, sizeof(timeStr), "    %02d:%02d %s    ", 
                             displayHour, timeinfo.tm_min, ampm);
                    
                    String l1 = String(timeStr);
                    if (WiFi.status() != WL_CONNECTED) {
                        l1.setCharAt(0, '!'); // '!' indicates offline
                    }
                    bool isTimerRunning = isCountdownTimerActive();
                    int cardOffset = isTimerRunning ? 1 : 0;
                    int timerCard = isTimerRunning ? 1 : -1;
                    int weatherCard1 = 1 + cardOffset;
                    int weatherCard2 = 2 + cardOffset;
                    int weatherCard3 = 3 + cardOffset;
                    int curCard = 4 + cardOffset;
                    int gcalCard = isGCalEnabled() ? curCard++ : -1;
                    int schoolCard = schoolCalendarEnabled ? curCard++ : -1;
                    int calcWmataIdx = curCard;

                    if (sunriseActive) {
                        l1.setCharAt(0, '\x06'); // sun icon on pos 0
                    } else if (weatherEnabled && weatherFetched) {
                        int totalScreens = curCard + (wmataVisible ? 1 : 0);
                        if (clockScreenIndex >= 1 && clockScreenIndex < totalScreens) {
                            if (clockScreenIndex != lastCreatedCharScreenIndex) {
                                lastCreatedCharScreenIndex = clockScreenIndex;
                                setScreenDotCustomChar(clockScreenIndex);
                            }
                            l1.setCharAt(0, '\x07');
                        } else {
                            l1.setCharAt(0, ' ');
                        }
                    } else {
                        l1.setCharAt(0, ' ');
                    }
                    if (sunriseActive) {
                        l1.setCharAt(14, '\x06'); // sun icon next to bell
                        if (weatherAlertActive) {
                            l1.setCharAt(13, '\x05'); // weather alert next to sun
                        } else if (isTimerRunning) {
                            l1.setCharAt(13, '\x04'); // hourglass
                        } else if (sleepTimerEnd > 0) {
                            l1.setCharAt(13, '\x03'); // zZ sleep
                        } else {
                            l1.setCharAt(13, ' ');
                        }
                    } else {
                        if (weatherAlertActive) {
                            l1.setCharAt(14, '\x05'); // weather alert right next to bell!
                            if (isTimerRunning) {
                                l1.setCharAt(13, '\x04');
                            } else if (sleepTimerEnd > 0) {
                                l1.setCharAt(13, '\x03');
                            } else {
                                l1.setCharAt(13, ' ');
                            }
                        } else {
                            if (isTimerRunning && sleepTimerEnd > 0) {
                                l1.setCharAt(14, '\x03'); // zZ sleep
                                l1.setCharAt(13, '\x04'); // hourglass timer
                            } else if (isTimerRunning) {
                                l1.setCharAt(14, ' ');
                                l1.setCharAt(13, '\x04'); // hourglass timer
                            } else if (sleepTimerEnd > 0) {
                                l1.setCharAt(14, ' ');
                                l1.setCharAt(13, '\x03'); // zZ sleep
                            } else {
                                l1.setCharAt(14, ' ');
                                l1.setCharAt(13, ' ');
                            }
                        }
                    }
                    if (alarmEnabled || snoozeActive) {
                        static bool snoozeBlinkToggle = false;
                        if (snoozeActive) {
                            snoozeBlinkToggle = !snoozeBlinkToggle;
                            if (snoozeBlinkToggle) {
                                l1.setCharAt(15, ' ');
                            } else {
                                l1.setCharAt(15, '\x08');
                            }
                        } else {
                            l1.setCharAt(15, '\x08');
                        }
                    } else {
                        l1.setCharAt(15, ' ');
                    }
                    
                    String l2_str = "";
                    
                    // Special 2-line rendering override for Screen (WMATA Bus Tracker)
                    if (clockScreenIndex == calcWmataIdx && wmataVisible) {
                        // Keep the time on line 1; show route + next prediction on line 2
                        if (clockScreenIndex != lastCreatedCharScreenIndex) {
                            lastCreatedCharScreenIndex = clockScreenIndex;
                            setScreenDotCustomChar(clockScreenIndex);
                        }
                        if (l1.length() > 0) {
                            l1.setCharAt(0, '\x07'); // bus icon
                        }

                        if (wmataDataValid) {
                            String route = getWmataLine1();
                            int atIdx = route.indexOf(" @");
                            if (atIdx >= 0) {
                                route = route.substring(0, atIdx);
                                route.trim();
                            }
                            if (route.isEmpty()) route = "Bus";

                            String pred = getWmataLine2();
                            pred.replace("Next: ", "");
                            pred.trim();

                            char wmataBuf[17];
                            snprintf(wmataBuf, sizeof(wmataBuf), "%s: %s", route.c_str(), pred.c_str());
                            l2_str = String(wmataBuf);
                        } else {
                            l2_str = "Bus: Loading...";
                        }
                        while (l2_str.length() < 16) l2_str += " ";
                        wmataHandled = true;
                    }
                    
                    if (now < volumeDisplayTimeout) {
                        char volBuf[17];
                        volBuf[0] = '[';
                        if (isMuted) {
                            snprintf(volBuf, sizeof(volBuf), "[    MUTED     ]");
                        } else {
                            int volVal = (currentState == STATE_MUSIC_MODE) ? getSpotifyVolumePercent() : currentVolume;
                            int maxVal = (currentState == STATE_MUSIC_MODE) ? 100 : 21;
                            int numBlocks = (volVal * 14) / maxVal;
                            for (int i = 1; i <= 14; i++) {
                                if (i <= numBlocks) volBuf[i] = (char)0xFF;
                                else volBuf[i] = '-';
                            }
                            volBuf[15] = ']';
                            volBuf[16] = '\0';
                        }
                        l2_str = String(volBuf);
                    } else if (!wmataHandled) {
                        const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                        const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                        
                        bool showPages = (weatherEnabled && weatherFetched) || isTimerRunning;
                        
                        if (showPages) {
                            String body = "";
                            bool isEvening = (timeinfo.tm_hour >= 19); // 7:00 PM evening flip to tomorrow

                            if (clockScreenIndex == 0) {
                                bool isWeekday = (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5);
                                bool isMorning = (timeinfo.tm_hour >= 6 && timeinfo.tm_hour < 12);
                                String dayOffName = schoolCalendarEnabled ? getTodaySchoolDayOffName(&timeinfo) : "";
                                if (isWeekday && isMorning && !dayOffName.isEmpty()) {
                                    char noSchoolBuf[17];
                                    if (dayOffName.length() <= 5) {
                                        snprintf(noSchoolBuf, sizeof(noSchoolBuf), "No School: %s", dayOffName.c_str());
                                    } else {
                                        snprintf(noSchoolBuf, sizeof(noSchoolBuf), "No School Today!");
                                    }
                                    body = String(noSchoolBuf);
                                } else {
                                    body = String(days[timeinfo.tm_wday]) + ", " + months[timeinfo.tm_mon] + ". " + String(timeinfo.tm_mday);
                                }
                            } else if (clockScreenIndex == timerCard && isTimerRunning) {
                                TimerSnapshot ts = getTimerSnapshot();
                                char progBuf[17];
                                progBuf[0] = '[';
                                int numBlocks = (int)(ts.progress * 14.0f + 0.5f);
                                if (numBlocks < 0) numBlocks = 0;
                                if (numBlocks > 14) numBlocks = 14;
                                for (int i = 1; i <= 14; i++) {
                                    if (i <= numBlocks) progBuf[i] = (char)0xFF;
                                    else progBuf[i] = '-';
                                }
                                progBuf[15] = ']';
                                progBuf[16] = '\0';
                                body = String(progBuf);

                                char t1Buf[17];
                                snprintf(t1Buf, sizeof(t1Buf), "Timer %s Left", ts.timeString.c_str());
                                l1 = padQuickActionLine(String(t1Buf));
                            } else if (clockScreenIndex == weatherCard1) {
                                char weatherBuf[17];
                                snprintf(weatherBuf, sizeof(weatherBuf), "%d\xDF" "F %s", weatherTemp, weatherLabel.c_str());
                                body = String(weatherBuf);
                            } else if (clockScreenIndex == weatherCard2) {
                                char forecastBuf[17];
                                if (weatherAlertActive && !weatherAlertTitle.isEmpty()) {
                                    snprintf(forecastBuf, sizeof(forecastBuf), "%.16s", weatherAlertTitle.c_str());
                                } else {
                                    if (isEvening) {
                                        int dispHigh = weatherTomorrowHighTemp;
                                        int dispFeels = weatherTomorrowFeelsLike > 0 ? weatherTomorrowFeelsLike : dispHigh;
                                        snprintf(forecastBuf, sizeof(forecastBuf), "Tmw %d\xDF Fl %d\xDF", dispHigh, dispFeels);
                                    } else {
                                        int dispHigh = weatherHighTemp;
                                        int dispFeels = weatherFeelsLike > 0 ? weatherFeelsLike : dispHigh;
                                        if (dispHigh >= 100 || dispFeels >= 100) {
                                            snprintf(forecastBuf, sizeof(forecastBuf), "Hi %d\xDF Fl %d\xDF", dispHigh, dispFeels);
                                        } else {
                                            snprintf(forecastBuf, sizeof(forecastBuf), "Hi %d\xDF Feels %d\xDF", dispHigh, dispFeels);
                                        }
                                    }
                                }
                                body = String(forecastBuf);
                            } else if (clockScreenIndex == weatherCard3) {
                                char precipBuf[17];
                                int activeCode = isEvening ? weatherTomorrowCodeDaily : weatherCodeDaily;
                                bool isSnowy = (activeCode == 71 || activeCode == 73 || 
                                                activeCode == 75 || activeCode == 77 || 
                                                activeCode == 85 || activeCode == 86);
                                const char* precName = isSnowy ? "Snow" : "Rain";
                                int activePrecipChance = isEvening ? weatherTomorrowPrecipChance : weatherPrecipitationChance;
                                int dispUV = isEvening ? weatherTomorrowUVIndex : weatherUVIndex;
                                int activeRainStartHour = isEvening ? weatherTomorrowRainStartHour : weatherRainStartHour;
                                
                                if (activePrecipChance >= 35 && activeRainStartHour >= 0) {
                                    int hr12 = activeRainStartHour % 12;
                                    if (hr12 == 0) hr12 = 12;
                                    const char* ampm = (activeRainStartHour < 12) ? "a" : "p";
                                    
                                    int curHour = timeinfo.tm_hour;
                                    
                                    if (isEvening) {
                                        if (hr12 >= 10) {
                                            snprintf(precipBuf, sizeof(precipBuf), "Tmw %s %d%s %d%%", precName, hr12, ampm, activePrecipChance);
                                        } else {
                                            snprintf(precipBuf, sizeof(precipBuf), "Tmw %s %d%s (%d%%)", precName, hr12, ampm, activePrecipChance);
                                        }
                                    } else {
                                        if (curHour == activeRainStartHour) {
                                            snprintf(precipBuf, sizeof(precipBuf), "%s Now (%d%%)", precName, activePrecipChance);
                                        } else {
                                            snprintf(precipBuf, sizeof(precipBuf), "%s %d%s (%d%%)", precName, hr12, ampm, activePrecipChance);
                                        }
                                    }
                                } else if (dispUV >= 3) {
                                    const char* uvCategory = "Mod";
                                    if (dispUV >= 11) uvCategory = "Extr";
                                    else if (dispUV >= 8) uvCategory = "V.Hi";
                                    else if (dispUV >= 6) uvCategory = "High";
                                    
                                    if (isEvening) {
                                        snprintf(precipBuf, sizeof(precipBuf), "Tmw UV %d (%s)", dispUV, uvCategory);
                                    } else {
                                        snprintf(precipBuf, sizeof(precipBuf), "Peak UV %d (%s)", dispUV, uvCategory);
                                    }
                                } else {
                                    if (isEvening) {
                                        snprintf(precipBuf, sizeof(precipBuf), "Tmw Dry (%d%%)", activePrecipChance);
                                    } else {
                                        snprintf(precipBuf, sizeof(precipBuf), "Dry Day (%d%%)", activePrecipChance);
                                    }
                                }
                                body = String(precipBuf);
                            } else if (clockScreenIndex == gcalCard && isGCalEnabled()) {
                                GCalSnapshot gSnap = getGCalSnapshot();
                                if (gSnap.loaded) {
                                    body = gSnap.nextEvent.displayString;
                                    if (body.isEmpty()) body = getGCalNoEventsMessage(gSnap.lookaheadHours);
                                } else {
                                    body = "Loading agenda";
                                }
                            } else if (clockScreenIndex == schoolCard && schoolCalendarEnabled) {
                                if (schoolCalendarLoaded) {
                                    int schoolDays = -1;
                                    String nextDayOff = getNextSchoolDayOff(&timeinfo, &schoolDays);
                                    int closestDays = -1;
                                    String closestName = "";
                                    for (int i = 0; i < MAX_COUNTDOWN_EVENTS; i++) {
                                        if (countdownEvents[i].enabled && countdownEvents[i].year > 0) {
                                            int d = daysUntilDate(countdownEvents[i].year, countdownEvents[i].month, countdownEvents[i].day, &timeinfo);
                                            if (d >= 0 && (closestDays < 0 || d < closestDays)) {
                                                closestDays = d;
                                                closestName = countdownEvents[i].name;
                                            }
                                        }
                                    }
                                    bool showCountdown = (closestDays >= 0 && (schoolDays < 0 || closestDays <= schoolDays));
                                    if (showCountdown) {
                                        char cntBuf[17];
                                        if (closestDays == 0) {
                                            snprintf(cntBuf, sizeof(cntBuf), "%s!!", closestName.c_str());
                                        } else if (closestDays == 1) {
                                            snprintf(cntBuf, sizeof(cntBuf), "T-1 %s", closestName.c_str());
                                        } else {
                                            snprintf(cntBuf, sizeof(cntBuf), "%dd %s", closestDays, closestName.c_str());
                                        }
                                        body = String(cntBuf);
                                    } else if (nextDayOff == "None") {
                                        body = "No days off";
                                    } else {
                                        body = "No Sch " + nextDayOff;
                                    }
                                } else {
                                    body = "Loading calendar";
                                }
                            } else {
                                body = String(days[timeinfo.tm_wday]) + ", " + months[timeinfo.tm_mon] + ". " + String(timeinfo.tm_mday);
                            }
                            
                            int spaces = (16 - body.length()) / 2;
                            if (spaces < 0) spaces = 0;
                            l2_str = "";
                            for (int i = 0; i < spaces; i++) l2_str += " ";
                            l2_str += body;
                            while (l2_str.length() < 16) l2_str += " ";
                        } else {
                            String dateRaw = String(days[timeinfo.tm_wday]) + ", " + months[timeinfo.tm_mon] + ". " + String(timeinfo.tm_mday);
                            int spaces = (16 - dateRaw.length()) / 2;
                            if (spaces < 0) spaces = 0;
                            
                            l2_str = "";
                            for (int i = 0; i < spaces; i++) l2_str += " ";
                            l2_str += dateRaw;
                            while (l2_str.length() < 16) l2_str += " ";
                        }
                    }
                    
                    displayLines(l1, l2_str);
                } else {
                    displayLines("NTP Error", "Sync lost...");
                }
            }
            break;
        }
        
        case STATE_MUSIC_MODE: {
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[Music] WiFi lost in music mode. Stopping Spotify.");
                stopAudioPlayback();
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
                break;
            }
            
            if (timeAvailable) {
                bool triggerAlarm = false;
                if (snoozeActive) {
                    if (timeinfo.tm_hour == snoozeHour && timeinfo.tm_min == snoozeMinute) {
                        triggerAlarm = true;
                    }
                } else if (shouldAlarmTrigger(timeinfo)) {
                    triggerAlarm = true;
                }
                
                if (triggerAlarm) {
                    if (!alarmTriggeredThisMinute) {
                        Serial.println("[Alarm] Alarm triggered!");
                        alarmTriggeredThisMinute = true;
                        snoozeActive = false;
                        currentState = STATE_ALARM_RINGING;
                        startAlarmPlayback();
                        break;
                    }
                } else {
                    alarmTriggeredThisMinute = false;
                }
            }
            
            if (now - lastLCDUpdate >= 400) {
                lastLCDUpdate = now;
                
                if (timeAvailable) {
                    int displayHour = timeinfo.tm_hour;
                    const char* ampm = "AM";
                    if (displayHour >= 12) ampm = "PM";
                    if (displayHour > 12) displayHour -= 12;
                    if (displayHour == 0) displayHour = 12;
                    
                    char timeStr[17];
                    snprintf(timeStr, sizeof(timeStr), "    %02d:%02d %s    ", displayHour, timeinfo.tm_min, ampm);
                    
                    setMusicNoteCustomChar();
                    String l1 = String(timeStr);
                    l1.setCharAt(0, '\x06'); // 🎵 Music Note Icon!
                    if (getSpotifyIsPlaying()) {
                        l1.setCharAt(14, '\x01'); // ▶ Play icon
                    } else {
                        l1.setCharAt(14, '\x02'); // ⏸ Pause icon
                    }
                    if (alarmEnabled) {
                        l1.setCharAt(15, '\x08'); // 🔔 Alarm
                    } else {
                        l1.setCharAt(15, ' ');
                    }
                    if (isCountdownTimerActive()) {
                        l1.setCharAt(13, '\x04'); // ⏳ Bedside Timer
                    } else if (sleepTimerEnd > 0 && sleepTimerEnd > now) {
                        l1.setCharAt(13, '\x03'); // zZ Sleep Icon!
                    } else {
                        l1.setCharAt(13, ' ');
                    }

                    
                    String l2_str = "";
                    if (now < volumeDisplayTimeout) {
                        char volBuf[17];
                        if (isMuted) {
                            snprintf(volBuf, sizeof(volBuf), "[     MUTED    ]");
                        } else {
                            volBuf[0] = '[';
                            int numBlocks = (currentState == STATE_MUSIC_MODE) ? ((getSpotifyVolumePercent() * 14) / 100) : ((currentVolume * 14) / 21);
                            for (int i = 1; i <= 14; i++) {
                                if (i <= numBlocks) volBuf[i] = 0xFF;
                                else volBuf[i] = '-';
                            }
                            volBuf[15] = ']';
                            volBuf[16] = '\0';
                        }
                        l2_str = String(volBuf);
                    } else {
                        if (strlen(SPOTIFY_CLIENT_ID) == 0 || strlen(SPOTIFY_REFRESH_TOKEN) == 0) {
                            l2_str = " Setup config.h ";
                        } else {
                            String musicText = getSpotifyTrackText();
                            static int scrollIndex = 0;
                            String formattedTrack = String((char)6) + " " + musicText;
                            int textLen = formattedTrack.length();
                            
                            if (textLen <= 16) {
                                int spaces = (16 - textLen) / 2;
                                l2_str = "";
                                for (int i = 0; i < spaces; i++) l2_str += " ";
                                l2_str += formattedTrack;
                                while (l2_str.length() < 16) l2_str += " ";
                            } else {
                                String fullScroll = formattedTrack + "   ";
                                textLen = fullScroll.length();
                                l2_str = fullScroll.substring(scrollIndex, scrollIndex + 16);
                                if (l2_str.length() < 16) {
                                    l2_str += fullScroll.substring(0, 16 - l2_str.length());
                                }
                                scrollIndex = (scrollIndex + 1) % textLen;
                            }
                        }
                    }
                    
                    displayLines(l1, l2_str);
                } else {
                    displayLines("NTP Error", "Sync lost...");
                }
            }
            break;
        }
        
        case STATE_ALARM_RINGING: {
            if (now - lastLCDUpdate >= 350) {
                lastLCDUpdate = now;
                setHeartCustomChar();
                static bool flashToggle = false;
                flashToggle = !flashToggle;
                
                String l1 = flashToggle ? " Rise & Shine \x07 " : "  Rise & Shine  ";
                String l2 = "";
                if (snoozeDurationSelection == 0) {
                    l2 = "> [ Dismiss ] < ";
                } else if (snoozeDurationSelection == 5) {
                    l2 = "> [Snooze  5m] <";
                } else if (snoozeDurationSelection == 10) {
                    l2 = "> [Snooze 10m] <";
                } else if (snoozeDurationSelection == 15) {
                    l2 = "> [Snooze 15m] <";
                }
                
                displayLines(l1, l2);
            }
            break;
        }
        
        case STATE_SETTINGS_MENU: {
            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                renderSettingsMenu();
            }
            break;
        }
        
        case STATE_NOISE_MACHINE: {
            
            if (timeAvailable) {
                bool triggerAlarm = false;
                if (snoozeActive) {
                    if (timeinfo.tm_hour == snoozeHour && timeinfo.tm_min == snoozeMinute) {
                        triggerAlarm = true;
                    }
                } else if (shouldAlarmTrigger(timeinfo)) {
                    triggerAlarm = true;
                }
                
                if (triggerAlarm) {
                    if (!alarmTriggeredThisMinute) {
                        Serial.println("[Alarm] Alarm triggered!");
                        alarmTriggeredThisMinute = true;
                        snoozeActive = false;
                        currentState = STATE_ALARM_RINGING;
                        startAlarmPlayback();
                        break;
                    }
                } else {
                    alarmTriggeredThisMinute = false;
                }
            }
            
            if (now - lastLCDUpdate >= 1000) {
                lastLCDUpdate = now;
                
                String l1 = "  Sleep Noise   ";
                if (sleepTimerEnd > 0) {
                    int remainingMin = (sleepTimerEnd - now) / 60000 + 1;
                    if (remainingMin < 1) remainingMin = 1;
                    char timeBuf[17];
                    snprintf(timeBuf, sizeof(timeBuf), "Sleep Noise %2dm", remainingMin);
                    l1 = String(timeBuf);
                }
                
                String l2_str = "";
                if (now < volumeDisplayTimeout) {
                    char volBuf[17];
                    volBuf[0] = '[';
                    int numBlocks = (currentVolume * 14) / 21;
                    for (int i = 1; i <= 14; i++) {
                        if (i <= numBlocks) volBuf[i] = 0xFF;
                        else volBuf[i] = '-';
                    }
                    volBuf[15] = ']';
                    volBuf[16] = '\0';
                    l2_str = String(volBuf);
                } else {
                    if (!audio.isRunning()) {
                        l2_str = "    [PAUSED]    ";
                    } else {
                        l2_str = "   [PLAYING]    ";
                    }
                }
                
                displayLines(l1, l2_str);
            }
            break;
        }
        
        case STATE_ALERT_MESSAGE: {
            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                String l2 = "";
                
                if (intercomMessage.length() <= 16) {
                    l2 = intercomMessage;
                    while (l2.length() < 16) l2 += " ";
                } else {
                    static int scrollIndex = 0;
                    String extendedMsg = intercomMessage + "    " + intercomMessage;
                    l2 = extendedMsg.substring(scrollIndex, scrollIndex + 16);
                    scrollIndex = (scrollIndex + 1) % (intercomMessage.length() + 4);
                }
                
                displayLines(alertHeader, l2);
            }
            break;
        }

        case STATE_VOICE_MEMO_PROMPT: {
            if (now >= voiceMemoPromptTimeout) {
                Serial.println("[Voice Memo] Prompt timeout. Dismissing.");
                stopAudioPlayback();
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
                break;
            }

            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                String l1 = (voiceMemoPromptItem == 0 ? "> " : "  ") + String("1. Replay");
                String l2 = (voiceMemoPromptItem == 1 ? "> " : "  ") + String("2. Dismiss");
                while (l1.length() < 16) l1 += " ";
                while (l2.length() < 16) l2 += " ";
                displayLines(l1, l2);
            }
            break;
        }
        
        case STATE_SUNRISE_CANCEL_PROMPT: {
            if (now >= sunriseCancelPromptTimeout) {
                Serial.println("[Sunrise] Prompt timeout. Returning to clock.");
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
                break;
            }
            
            if (now - lastLCDUpdate >= 500) {
                lastLCDUpdate = now;
                displayLines("Cancel Alarm?   ", "[ Push Confirm ]");
            }
            break;
        }
        
        case STATE_SNOOZE_CANCEL_PROMPT: {
            if (now >= snoozeCancelPromptTimeout) {
                Serial.println("[Snooze] Prompt timeout. Returning to clock.");
                currentState = STATE_IDLE_CLOCK;
                lastLCDUpdate = 0;
                break;
            }
            
            if (now - lastLCDUpdate >= 500) {
                lastLCDUpdate = now;
                displayLines("Cancel Snooze?  ", "[ Push Confirm ]");
            }
            break;
        }
        
        case STATE_OFFLINE_PROMPT: {
            if (now - lastLCDUpdate >= 500) {
                lastLCDUpdate = now;
                displayLines("WiFi Timeout!   ", "Push to Setup   ");
            }
            break;
        }
        
        case STATE_OFFLINE_SET_HOUR: {
            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                auto format12H = [](int hr) -> String {
                    if (hr == 0) return String("12 AM");
                    if (hr == 12) return String("12 PM");
                    if (hr < 12) return String(hr) + " AM";
                    return String(hr - 12) + " PM";
                };
                displayLines("Set Clock Hour", "   > " + format12H(offlineSetupHour) + " <   ");
            }
            break;
        }
        
        case STATE_OFFLINE_SET_MIN: {
            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                char minBuf[17];
                snprintf(minBuf, sizeof(minBuf), "     > %02d <     ", offlineSetupMin);
                displayLines("Set Clock Min", minBuf);
            }
            break;
        }

        case STATE_QUICK_ACTIONS: {
            if (now - lastLCDUpdate >= 250) {
                lastLCDUpdate = now;
                renderQuickActions();
            }
            break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("====================================");
    Serial.println("  Spotify Clock " + String(FIRMWARE_VERSION) + "  ");
    Serial.println("====================================");
    Serial.println();

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(100);
    Serial.printf("[System] I2C Bus initialized on SDA=%d, SCL=%d\r\n", I2C_SDA_PIN, I2C_SCL_PIN);
    
    setupDisplay();
    
    scanI2CBus();

    displayLines("Booting...", "Initializing...");

    loadSettings();

#ifdef SIMULATION_MODE
    // Wokwi Special Calendar test value from config.h
    char simName[32] = {0};
    int cntY = 0, cntM = 0, cntD = 0;
    sscanf(SIM_COUNTDOWN, "%31[^,],%d-%d-%d", simName, &cntY, &cntM, &cntD);
    for (int i = 0; i < MAX_COUNTDOWN_EVENTS; i++) { countdownEvents[i].enabled = false; }
    countdownEvents[0].enabled = (strlen(simName) > 0);
    countdownEvents[0].name = simName;
    countdownEvents[0].year = cntY;
    countdownEvents[0].month = cntM;
    countdownEvents[0].day = cntD;
    Serial.printf("[Simulation] Config countdown: %s %04d-%02d-%02d\r\n",
                  simName, cntY, cntM, cntD);
#endif

    setupWeather();
    setupSchoolCalendar();
    setupGCal();
    setupWmata();
    setupTimerManager();

    setupSPIFFS();

#ifdef SIMULATION_MODE
    pinMode(SIM_ENCODER_CLK, INPUT);
    pinMode(SIM_ENCODER_DT, INPUT);
    pinMode(SIM_ENCODER_SW, INPUT_PULLUP);
    Serial.println("[System] Initialized simulation GPIO encoder.");
#else
    if (!ss.begin(SEESAW_I2C_ADDR)) {
        seesawInitialized = false;
        Serial.printf("[Warning] Seesaw Encoder not found at address 0x%02X. Proceeding in headless mode.\r\n", SEESAW_I2C_ADDR);
    } else {
        seesawInitialized = true;
        Serial.println("[System] Seesaw Encoder initialized.");
        ss.pinMode(24, INPUT_PULLUP);
        lastEncoderPosition = ss.getEncoderPosition();
    }
#endif

    setupAudio();

#ifdef SIMULATION_MODE
    WiFi.begin("Wokwi-GUEST", "");
    Serial.println("[WiFi] Starting connection to virtual SSID: Wokwi-GUEST");
#else
    IPAddress dns(8, 8, 8, 8);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Starting connection to SSID: %s (strongest AP auto-select enabled)\r\n", WIFI_SSID);
#endif
    
    initSpotifyClient();
    setupSinricPro();
    
    // Configure Task Watchdog to 10.0 seconds (prevent false-positive resets during SSL handshakes)
    esp_task_wdt_init(10, true);
    Serial.println("[System] Task Watchdog reconfigured to 10.0 seconds.");

    xTaskCreate(
        spotifyTask,
        "spotifyTask",
        16384,
        NULL,
        1,
        NULL
    );
    xTaskCreatePinnedToCore(
        networkPollTask,
        "networkPollTask",
        16384,
        NULL,
        0, // Priority 0: time-slices cooperatively with Core 0 IDLE task
        NULL,
        0
    );
    
    currentState = STATE_DISCONNECTED;
    disconnectedStartTime = millis();

    Serial.printf("[HEAP] Free heap after setup: %u bytes (min: %u)\r\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

void checkWifiStatusAndReconnect(unsigned long now) {
    if (currentState == STATE_DISCONNECTED || 
        currentState == STATE_SYNCING_TIME ||
        currentState == STATE_OFFLINE_PROMPT ||
        currentState == STATE_OFFLINE_SET_HOUR ||
        currentState == STATE_OFFLINE_SET_MIN) {
        return;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastWifiReconnectAttempt >= 60000) {
            lastWifiReconnectAttempt = now;
            Serial.println("[WiFi] Connection lost! Initiating background reconnection...");
            WiFi.disconnect();
            #ifdef SIMULATION_MODE
            WiFi.begin("Wokwi-GUEST", "");
            #else
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            #endif
        }
    }
}

#include "sinric_manager.h"

void loop() {
    unsigned long now = millis();
    checkWifiStatusAndReconnect(now);
    
    if (WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.handle();
        updateWebServer();
        handleSinricPro();
    }
    audio.loop();

    if (pendingSaveSettingsTime > 0 && now >= pendingSaveSettingsTime) {
        pendingSaveSettingsTime = 0;
        saveSettings();
    }

    if (currentState == STATE_ALARM_RINGING && gentleWakeActive) {
        unsigned long elapsed = now - gentleWakeStartTime;
        int targetVol = (elapsed * alarmVolume) / 30000;
        if (targetVol > alarmVolume) {
            targetVol = alarmVolume;
            gentleWakeActive = false;
        }
        if (targetVol != currentVolume) {
            currentVolume = targetVol;
            updateAudioVolume();
            lastLCDUpdate = 0;
        }
    }

    if (sleepTimerEnd > 0 && now >= sleepTimerEnd) {
        sleepTimerEnd = 0;
        sleepDurationMinutes = 0;
        Serial.println("[System] Sleep timer expired. Stopping playback.");
        
        // Pause Spotify and stop local audio unconditionally
        {
            SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
            postSpotifyCommand(cmd);
        }
        stopAudioPlayback();
        
        if (currentState == STATE_MUSIC_MODE || currentState == STATE_NOISE_MACHINE) {
            currentState = STATE_IDLE_CLOCK;
        }
        lastLCDUpdate = 0;
    }

    static unsigned long timerBannerTimeout = 0;
    if (checkTimerJustFinished()) {
        Serial.println("[Timer] Countdown reached 00:00! Triggering siren sound & banner.");
        playNamedSound("siren");
        displayLines("  Timer Done!   ", "   Great Job!   ");
        timerBannerTimeout = now + 4000;
        lastLCDUpdate = now + 4000; // Hold banner on LCD for 4s without blocking audio decoding
        clockScreenIndex = 0;
    }
    if (timerBannerTimeout > 0 && now >= timerBannerTimeout) {
        timerBannerTimeout = 0;
        lastLCDUpdate = 0;
    }

    if (previewStopTimeout > 0 && now >= previewStopTimeout) {
        previewStopTimeout = 0;
        Serial.println("[Mock Audio] Preview stopped.");
        audio.stopSong();
        lastLCDUpdate = 0;
    }

#ifdef SIMULATION_MODE
    pollSimulationEncoder();
#else
    if (seesawInitialized && (now - lastSeesawPoll >= SEESAW_POLL_INTERVAL)) {
        lastSeesawPoll = now;
        pollSeesawEncoder();
    }
#endif

    handleSerialCommands();

    static unsigned long lastSystemStateManage = 0;
    if (now - lastSystemStateManage >= 50) {
        lastSystemStateManage = now;
        manageSystemStates();
    }
}

// ==========================================
// ESP32-audioI2S callback functions
// ==========================================
void audio_info(const char *info) {
    Serial.print("[Audio Info] ");
    Serial.println(info);
}

void audio_showstation(const char *info) {
    Serial.print("[Audio Station] ");
    Serial.println(info);
}

void audio_showstreamtitle(const char *info) {
    Serial.print("[Audio Title] ");
    Serial.println(info);
}

void audio_eof_mp3(const char *info) {
    Serial.print("[Audio EOF] ");
    Serial.println(info);
    if (currentState == STATE_ALARM_RINGING || currentState == STATE_MUSIC_MODE) {
        Serial.printf("[Audio] Looping local file: %s\r\n", ALARM_SOUNDS[alarmSoundIndex].path);
        startAudioPlayback(ALARM_SOUNDS[alarmSoundIndex].path, true);
    } else if (currentState == STATE_NOISE_MACHINE) {
        Serial.println("[Audio] Looping brown noise.");
        startAudioPlayback("/brown_noise.wav", true);
    }
}

void audio_eof_speech(const char *info) {
    audio_eof_mp3(info);
}

void audio_eof_stream(const char *info) {
    audio_eof_mp3(info);
}
