#include "display_manager.h"

// Define display buffering
String lastLine1 = "";
String lastLine2 = "";

// Custom characters definitions
byte bellChar[8] = {
    0b00100,
    0b01110,
    0b01110,
    0b01110,
    0b11111,
    0b00000,
    0b00100,
    0b00000
};

byte playChar[8] = {
    0b00000,
    0b01000,
    0b01100,
    0b01110,
    0b01100,
    0b01000,
    0b00000,
    0b00000
};

byte pauseChar[8] = {
    0b00000,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b00000
};

byte noteChar[8] = {
    0b00100,
    0b00110,
    0b00101,
    0b00100,
    0b01100,
    0b11100,
    0b11100,
    0b01100
};

byte sleepChar[8] = {
    0b00111, //   ███  (small z / I top)
    0b00010, //    █
    0b00111, //   ███
    0b00000, //
    0b11110, // ████  (big Z)
    0b00100, //   █
    0b01000, //  █
    0b11110  // ████
};

byte hourglassChar[8] = {
    0b11111,
    0b10001,
    0b01010,
    0b00100,
    0b01010,
    0b10001,
    0b11111,
    0b00000
};

byte alertChar[8] = {
    0b01100, // stem (starts at top row)
    0b01100, // stem
    0b01100, // stem
    0b01100, // stem
    0b00000, // gap
    0b01100, // square dot top
    0b01100, // square dot bottom (sits on baseline)
    0b00000  // empty bottom row
};

byte sunChar[8] = {
    0b00100,
    0b10101,
    0b01110,
    0b11111,
    0b01110,
    0b10101,
    0b00100,
    0b00000
};

byte oneDotChar[8] = {
    0b01000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000
};

byte heartChar[8] = {
    0b00000,
    0b01010,
    0b11111,
    0b11111,
    0b01110,
    0b00100,
    0b00000,
    0b00000
};

// Global LCD objects
static Adafruit_LiquidCrystal adaLcd(0x20);
static LiquidCrystal_I2C i2cLcd(0x27, 16, 2);
static bool isAdafruitBackpack = false;
static bool lcdDetected = false;
static bool normalCustomCharsLoaded = false;
static bool musicNoteLoaded = false;

void setupDisplay() {
    pinMode(MOSFET_PWM_PIN, OUTPUT);
    uint8_t addrs[] = {0x20, 0x27, 0x3F};
    uint8_t foundAddr = 0x00;
    for (uint8_t a : addrs) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            foundAddr = a;
            lcdDetected = true;
            Serial.printf("[Display] Auto-detected LCD at I2C address 0x%02X\r\n", a);
            break;
        }
    }
    
    if (!lcdDetected) {
        Serial.println("[Display] No LCD display detected on I2C bus. Skipping LCD driver.");
        return;
    }

    if (foundAddr == 0x20) {
        isAdafruitBackpack = true;
        adaLcd.begin(16, 2);
        adaLcd.setBacklight(HIGH);
        adaLcd.clear();

        adaLcd.createChar(0, bellChar);
        adaLcd.createChar(1, playChar);
        adaLcd.createChar(2, pauseChar);
        adaLcd.createChar(3, sleepChar);
        adaLcd.createChar(4, hourglassChar);
        adaLcd.createChar(5, alertChar);
        adaLcd.createChar(6, sunChar);
        adaLcd.createChar(7, oneDotChar);
    } else {
        isAdafruitBackpack = false;
        i2cLcd = LiquidCrystal_I2C(foundAddr, 16, 2);
        i2cLcd.init();
        i2cLcd.backlight();
        i2cLcd.clear();

        i2cLcd.createChar(0, bellChar);
        i2cLcd.createChar(1, playChar);
        i2cLcd.createChar(2, pauseChar);
        i2cLcd.createChar(3, sleepChar);
        i2cLcd.createChar(4, hourglassChar);
        i2cLcd.createChar(5, alertChar);
        i2cLcd.createChar(6, sunChar);
        i2cLcd.createChar(7, oneDotChar);
    }
    normalCustomCharsLoaded = true;
}

void ensureNormalCustomChars() {
    if (!lcdDetected) return;
    if (normalCustomCharsLoaded) return;
    if (isAdafruitBackpack) {
        adaLcd.setCursor(0, 0); adaLcd.print("                ");
        adaLcd.setCursor(0, 1); adaLcd.print("                ");
        adaLcd.createChar(0, bellChar);
        adaLcd.createChar(1, playChar);
        adaLcd.createChar(2, pauseChar);
        adaLcd.createChar(3, sleepChar);
        adaLcd.createChar(4, hourglassChar);
        adaLcd.createChar(5, alertChar);
        adaLcd.createChar(6, sunChar);
        adaLcd.createChar(7, oneDotChar);
    } else {
        i2cLcd.setCursor(0, 0); i2cLcd.print("                ");
        i2cLcd.setCursor(0, 1); i2cLcd.print("                ");
        i2cLcd.createChar(0, bellChar);
        i2cLcd.createChar(1, playChar);
        i2cLcd.createChar(2, pauseChar);
        i2cLcd.createChar(3, sleepChar);
        i2cLcd.createChar(4, hourglassChar);
        i2cLcd.createChar(5, alertChar);
        i2cLcd.createChar(6, sunChar);
        i2cLcd.createChar(7, oneDotChar);
    }
    lastLine1 = "";
    lastLine2 = "";
    normalCustomCharsLoaded = true;
    musicNoteLoaded = false;
}

void displayLines(String line1, String line2) {
    if (!lcdDetected) return;
    ensureNormalCustomChars();
    if (currentState != STATE_MUSIC_MODE) {
        restoreSunCustomChar();
    }
    while (line1.length() < 16) line1 += " ";
    while (line2.length() < 16) line2 += " ";
    if (line1.length() > 16) line1 = line1.substring(0, 16);
    if (line2.length() > 16) line2 = line2.substring(0, 16);
    
    if (line1 != lastLine1) {
        if (isAdafruitBackpack) {
            adaLcd.setCursor(0, 0);
            adaLcd.print(line1);
        } else {
            i2cLcd.setCursor(0, 0);
            i2cLcd.print(line1);
        }
        lastLine1 = line1;
    }
    if (line2 != lastLine2) {
        if (isAdafruitBackpack) {
            adaLcd.setCursor(0, 1);
            adaLcd.print(line2);
        } else {
            i2cLcd.setCursor(0, 1);
            i2cLcd.print(line2);
        }
        lastLine2 = line2;
    }
}

void forceDisplayLines(String line1, String line2) {
    if (!lcdDetected) return;
    ensureNormalCustomChars();
    while (line1.length() < 16) line1 += " ";
    while (line2.length() < 16) line2 += " ";
    if (line1.length() > 16) line1 = line1.substring(0, 16);
    if (line2.length() > 16) line2 = line2.substring(0, 16);
    
    if (isAdafruitBackpack) {
        adaLcd.setCursor(0, 0);
        adaLcd.print(line1);
        adaLcd.setCursor(0, 1);
        adaLcd.print(line2);
    } else {
        i2cLcd.setCursor(0, 0);
        i2cLcd.print(line1);
        i2cLcd.setCursor(0, 1);
        i2cLcd.print(line2);
    }
    lastLine1 = line1;
    lastLine2 = line2;
}

bool isNightMode = false;

void updateNightModeState(int currentHour, int currentMin) {
    if (!autoDimmingEnabled) {
        isNightMode = false;
        return;
    }
    int curMins = currentHour * 60 + currentMin;
    int riseMins = (sunriseHour > 0 || sunriseMin > 0) ? (sunriseHour * 60 + sunriseMin) : (7 * 60);
    int setMins = (sunsetHour > 0 || sunsetMin > 0) ? (sunsetHour * 60 + sunsetMin) : (20 * 60);
    
    if (setMins > riseMins) {
        isNightMode = (curMins >= setMins || curMins < riseMins);
    } else {
        isNightMode = (curMins >= 20 * 60 || curMins < 7 * 60);
    }
}

void updateHardwareBrightness() {
    unsigned long now = millis();
    bool isTemporarilyWoken = (temporaryBacklightTimeout > 0 && now < temporaryBacklightTimeout) || 
                              (currentState == STATE_SETTINGS_MENU) || 
                              (currentState == STATE_QUICK_ACTIONS) || 
                              (currentState == STATE_ALARM_RINGING) || 
                              (currentState == STATE_ALERT_MESSAGE) || 
                              (currentState == STATE_SUNRISE_CANCEL_PROMPT) || 
                              (currentState == STATE_SNOOZE_CANCEL_PROMPT) ||
                              (currentState == STATE_DISCONNECTED) ||
                              (currentState == STATE_SYNCING_TIME) ||
                              (currentState == STATE_OFFLINE_PROMPT);

    bool displayShouldBeOn = backlightState && (!isNightMode || isTemporarilyWoken);

    static int s_lastBacklightOn = -1;
    if (s_lastBacklightOn != (int)displayShouldBeOn) {
        s_lastBacklightOn = (int)displayShouldBeOn;
        if (isAdafruitBackpack) {
            adaLcd.setBacklight(displayShouldBeOn ? HIGH : LOW);
        } else {
            if (displayShouldBeOn) i2cLcd.backlight();
            else i2cLcd.noBacklight();
        }
        Serial.printf("[Display] Backlight state changed to: %s (nightMode=%d)\r\n", displayShouldBeOn ? "ON" : "OFF", isNightMode);
    }
}

bool isDisplayShouldBeOn() {
    if (!backlightState) return false;
    unsigned long now = millis();
    bool isTemporarilyWoken = (temporaryBacklightTimeout > 0 && now < temporaryBacklightTimeout) || 
                              (currentState == STATE_SETTINGS_MENU) || 
                              (currentState == STATE_QUICK_ACTIONS) || 
                              (currentState == STATE_ALARM_RINGING) || 
                              (currentState == STATE_ALERT_MESSAGE) || 
                              (currentState == STATE_SUNRISE_CANCEL_PROMPT) || 
                              (currentState == STATE_SNOOZE_CANCEL_PROMPT) ||
                              (currentState == STATE_DISCONNECTED) ||
                              (currentState == STATE_SYNCING_TIME) ||
                              (currentState == STATE_OFFLINE_PROMPT);
    if (isTemporarilyWoken) return true;
    return !isNightMode;
}

void setBacklight(bool on) {
    backlightState = on;
    updateHardwareBrightness();
}

void clearDisplay() {
    if (isAdafruitBackpack) {
        adaLcd.clear();
    } else {
        i2cLcd.clear();
    }
    lastLine1 = "";
    lastLine2 = "";
}

void setHeartCustomChar() {
    byte heartChar[8] = {
        0b01010,
        0b11111,
        0b11111,
        0b11111,
        0b01110,
        0b00100,
        0b00000,
        0b00000
    };
    if (isAdafruitBackpack) {
        adaLcd.createChar(7, heartChar);
    } else {
        i2cLcd.createChar(7, heartChar);
    }
}

void setMusicNoteCustomChar() {
    if (musicNoteLoaded) return;
    if (isAdafruitBackpack) {
        adaLcd.createChar(6, noteChar);
    } else {
        i2cLcd.createChar(6, noteChar);
    }
    musicNoteLoaded = true;
}

void restoreSunCustomChar() {
    if (!musicNoteLoaded) return;
    if (isAdafruitBackpack) {
        adaLcd.createChar(6, sunChar);
    } else {
        i2cLcd.createChar(6, sunChar);
    }
    musicNoteLoaded = false;
}

void setSleepCustomChar() {
    if (isAdafruitBackpack) {
        adaLcd.createChar(3, sleepChar);
    } else {
        i2cLcd.createChar(3, sleepChar);
    }
}

// 8 custom characters for Trek 2x2 font (ArminJo / Alpenglow 2x2)
static const byte bigGlyph0[8] PROGMEM = { 0b11111, 0b11111, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000 }; // 0: Top bar
static const byte bigGlyph1[8] PROGMEM = { 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000 }; // 1: Left bar
static const byte bigGlyph2[8] PROGMEM = { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111, 0b11111 }; // 2: Bottom bar
static const byte bigGlyph3[8] PROGMEM = { 0b11111, 0b11111, 0b00011, 0b00011, 0b00011, 0b00011, 0b11111, 0b11111 }; // 3: Top/Right/Bottom
static const byte bigGlyph4[8] PROGMEM = { 0b11111, 0b11111, 0b11000, 0b11000, 0b11000, 0b11000, 0b11111, 0b11111 }; // 4: Top/Left/Bottom
static const byte bigGlyph5[8] PROGMEM = { 0b11111, 0b11111, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000 }; // 5: Top/Left
static const byte bigGlyph6[8] PROGMEM = { 0b00011, 0b00011, 0b00011, 0b00011, 0b00011, 0b00011, 0b11111, 0b11111 }; // 6: Right/Bottom
static const byte bigGlyph7[8] PROGMEM = { 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11111, 0b11111 }; // 7: Left/Bottom

// 2x2 matrix for numerals 0-9: {Row0_Col0, Row0_Col1, Row1_Col0, Row1_Col1}
static const uint8_t BIG_DIGIT_MAP[10][4] PROGMEM = {
    {5, 255,   7, 6},     // 0: [5][■] / [7][6]
    {0, 1,     2, 7},     // 1: [0][1] / [2][7]
    {0, 3,     4, 2},     // 2: [0][3] / [4][2]
    {0, 3,     2, 3},     // 3: [0][3] / [2][3]
    {1, 1,     0, 5},     // 4: [1][1] / [0][5]
    {4, 0,     2, 3},     // 5: [4][0] / [2][3]
    {5, 0,     4, 3},     // 6: [5][0] / [4][3]
    {0, 3,     ' ', 1},   // 7: [0][3] / [ ][1]
    {4, 3,     4, 3},     // 8: [4][3] / [4][3]
    {4, 3,     2, 6}      // 9: [4][3] / [2][6]
};

static void writeLcdByte(int col, int row, uint8_t b) {
    if (!lcdDetected) return;
    if (isAdafruitBackpack) {
        adaLcd.setCursor(col, row);
        adaLcd.write(b);
    } else {
        i2cLcd.setCursor(col, row);
        i2cLcd.write(b);
    }
}

static void loadBigCustomCharacters() {
    if (!lcdDetected) return;
    if (isAdafruitBackpack) {
        adaLcd.setCursor(0, 0); adaLcd.print("                ");
        adaLcd.setCursor(0, 1); adaLcd.print("                ");
    } else {
        i2cLcd.setCursor(0, 0); i2cLcd.print("                ");
        i2cLcd.setCursor(0, 1); i2cLcd.print("                ");
    }
    const byte* glyphs[8] = {bigGlyph0, bigGlyph1, bigGlyph2, bigGlyph3, bigGlyph4, bigGlyph5, bigGlyph6, bigGlyph7};
    for (int i = 0; i < 8; i++) {
        byte buf[8];
        memcpy_P(buf, glyphs[i], 8);
        if (isAdafruitBackpack) {
            adaLcd.createChar(i, buf);
        } else {
            i2cLcd.createChar(i, buf);
        }
    }
    lastLine1 = "";
    lastLine2 = "";
    normalCustomCharsLoaded = false;
}

static void drawBigDigit(int digit, int colStart) {
    if (digit < 0 || digit > 9) {
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 2; c++) {
                writeLcdByte(colStart + c, r, ' ');
            }
        }
        return;
    }
    uint8_t map[4];
    memcpy_P(map, BIG_DIGIT_MAP[digit], 4);
    writeLcdByte(colStart + 0, 0, map[0]);
    writeLcdByte(colStart + 1, 0, map[1]);
    writeLcdByte(colStart + 0, 1, map[2]);
    writeLcdByte(colStart + 1, 1, map[3]);
}

static int lastBigHour = -1;
static int lastBigMinute = -1;

void drawBigClock(int hour24, int minute) {
    if (!lcdDetected) return;

    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    bool isPM = (hour24 >= 12);

    // If time hasn't changed and Big Number custom characters are already active, skip redraw to eliminate flicker
    if (lastBigHour == hour12 && lastBigMinute == minute && !normalCustomCharsLoaded) {
        return;
    }
    lastBigHour = hour12;
    lastBigMinute = minute;

    if (normalCustomCharsLoaded) {
        loadBigCustomCharacters();
    }

    int h1 = hour12 / 10;
    int h2 = hour12 % 10;
    int m1 = minute / 10;
    int m2 = minute % 10;

    if (h1 > 0) {
        // Double-digit hour (10, 11, 12): [H1] [H2] [:] [M1] [M2] [AM/PM] (Every element spaced across 16 cols!)
        drawBigDigit(h1, 0); // Cols 0, 1
        writeLcdByte(2, 0, ' ');
        writeLcdByte(2, 1, ' ');
        drawBigDigit(h2, 3); // Cols 3, 4
        writeLcdByte(5, 0, ' ');
        writeLcdByte(5, 1, ' ');
        writeLcdByte(6, 0, ':');
        writeLcdByte(6, 1, ':');
        writeLcdByte(7, 0, ' ');
        writeLcdByte(7, 1, ' ');
        drawBigDigit(m1, 8);   // Cols 8, 9
        writeLcdByte(10, 0, ' ');
        writeLcdByte(10, 1, ' ');
        drawBigDigit(m2, 11);  // Cols 11, 12
        writeLcdByte(13, 0, ' ');
        writeLcdByte(13, 1, ' ');
    } else {
        // Single-digit hour (1 to 9): [  ][H][ ][:][ ][M1][ ][M2][  ][AM/PM]
        writeLcdByte(0, 0, ' ');
        writeLcdByte(0, 1, ' ');
        writeLcdByte(1, 0, ' ');
        writeLcdByte(1, 1, ' ');
        drawBigDigit(hour12, 2);      // Cols 2, 3
        writeLcdByte(4, 0, ' ');
        writeLcdByte(4, 1, ' ');
        writeLcdByte(5, 0, ':');
        writeLcdByte(5, 1, ':');
        writeLcdByte(6, 0, ' ');
        writeLcdByte(6, 1, ' ');
        drawBigDigit(m1, 7);          // Cols 7, 8
        writeLcdByte(9, 0, ' ');
        writeLcdByte(9, 1, ' ');
        drawBigDigit(m2, 10);         // Cols 10, 11
        writeLcdByte(12, 0, ' ');
        writeLcdByte(12, 1, ' ');
        writeLcdByte(13, 0, ' ');
        writeLcdByte(13, 1, ' ');
    }

    // Cols 14, 15: AM on Top Right (Row 0), PM on Bottom Right (Row 1)
    if (isPM) {
        writeLcdByte(14, 0, ' ');
        writeLcdByte(15, 0, ' ');
        writeLcdByte(14, 1, 'P');
        writeLcdByte(15, 1, 'M');
    } else {
        writeLcdByte(14, 0, 'A');
        writeLcdByte(15, 0, 'M');
        writeLcdByte(14, 1, ' ');
        writeLcdByte(15, 1, ' ');
    }

    // Invalidate the change buffer so the next normal draw refreshes fully
    lastLine1 = "BIG_CLOCK_L1";
    lastLine2 = "BIG_CLOCK_L2";
}

void setScreenDotCustomChar(int dotCount) {
    byte dotChar[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    
    // Left column: Dots 1 to 4 (bit 3)
    if (dotCount >= 1) dotChar[0] |= 0b01000;
    if (dotCount >= 2) dotChar[2] |= 0b01000;
    if (dotCount >= 3) dotChar[4] |= 0b01000;
    if (dotCount >= 4) dotChar[6] |= 0b01000;
    
    // Right column: Dots 5 to 8 (bit 1)
    if (dotCount >= 5) dotChar[0] |= 0b00010;
    if (dotCount >= 6) dotChar[2] |= 0b00010;
    if (dotCount >= 7) dotChar[4] |= 0b00010;
    if (dotCount >= 8) dotChar[6] |= 0b00010;
    
    if (isAdafruitBackpack) {
        adaLcd.createChar(7, dotChar);
    } else {
        i2cLcd.createChar(7, dotChar);
    }
}
