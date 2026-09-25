#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "globals.h"
#include <Adafruit_LiquidCrystal.h>

// Set up the LCD module, set pin mappings, backlight, and custom characters
void setupDisplay();

// Helper to write text to line 1 and line 2 of the LCD (with change-buffering)
void displayLines(String line1, String line2);

// Direct display writing (bypasses buffering, e.g. for loading states)
void forceDisplayLines(String line1, String line2);

// Turn off/on LCD backlight directly
void setBacklight(bool on);

// Update MOSFET PWM brightness (1-100%)
void updateHardwareBrightness();
bool isDisplayShouldBeOn();
void updateNightModeState(int currentHour, int currentMin);
extern bool isNightMode;

// Clears the LCD screen
void clearDisplay();

// Draw a large 2-line time on the whole display (3x2 digit font)
void drawBigClock(int hour24, int minute);

// Replaces Slot 7 with custom Heart icon
void setHeartCustomChar();

// Replaces Slot 3 with Music Note or Sleep icon
void setMusicNoteCustomChar();
void restoreSunCustomChar();
void setSleepCustomChar();

// Replaces Slot 7 with screen indicator dots (1-3 dots)
void setScreenDotCustomChar(int dotCount);

#endif // DISPLAY_MANAGER_H
