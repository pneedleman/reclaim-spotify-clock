#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

#define FIRMWARE_VERSION "v1.14"

// ==========================================
// WiFi Settings
// ==========================================
#include "secrets.h"

// Replace these with your actual Wi-Fi credentials
#define WIFI_SSID SECRET_WIFI_SSID
#define WIFI_PASSWORD SECRET_WIFI_PASSWORD

// ==========================================
// Spotify Settings
// ==========================================
// Spotify Standalone Web API credentials (obtained via get_refresh_token.py)
#define SPOTIFY_CLIENT_ID     SECRET_SPOTIFY_CLIENT_ID
#define SPOTIFY_CLIENT_SECRET SECRET_SPOTIFY_CLIENT_SECRET
#define SPOTIFY_REFRESH_TOKEN SECRET_SPOTIFY_REFRESH_TOKEN

// ==========================================
// Philips Hue Settings
// ==========================================
// Set HUE_BRIDGE_IP to your Hue Bridge's local IP address (e.g. "192.168.1.50").
// If you don't know it, leave blank for auto-discovery.
#define HUE_BRIDGE_IP       ""
#define HUE_BRIDGE_USERNAME SECRET_HUE_BRIDGE_USERNAME
#define HUE_GROUP_ID        "1"

// Fallback lat/lon (used if ip-api geolocation fails)
#define DEFAULT_LATITUDE  38.9959
#define DEFAULT_LONGITUDE -77.0424

// ==========================================
// Spotify Target Speaker Settings
// ==========================================
// Set DEFAULT_SPOTIFY_DEVICE_NAME to your speaker's name in Spotify (e.g. "Bedroom Echo" or "Clock Speaker")
#define DEFAULT_SPOTIFY_DEVICE_NAME ""
#define DEFAULT_SPOTIFY_DEVICE_ID   ""

// ==========================================
// DCPS School Calendar JSON URL
// Update this to your raw GitHub URL after enabling the GitHub Action
// ==========================================
#define DCPS_CALENDAR_JSON_URL "https://raw.githubusercontent.com/pneedleman/dcps_to_json/main/data/dcps_calendar.json"

// ==========================================
// NTP & Timezone Settings
// ==========================================
#define NTP_SERVER "pool.ntp.org"


// ==========================================
// Wokwi Simulation Mode (Optional) SIM MODE!!!
// ==========================================
// Uncomment the line below if you want to run this in Wokwi simulator
// #define SIMULATION_MODE


// Timezone string in TZ format.
// Default: US Eastern Time (New York)
// Adjust as needed. Reference: https://github.com/nayarsystems/posix_tz_db
#define DEFAULT_TIMEZONE "EST5EDT,M3.2.0,M11.1.0"

// ==========================================
// Hardware Address Definitions
// ==========================================
// Standard I2C address for LiquidCrystal I2C modules (usually 0x27 or 0x3F)
#define LCD_I2C_ADDR 0x27

// Standard I2C address for Adafruit Seesaw rotary encoder breakouts
#define SEESAW_I2C_ADDR 0x36

// ==========================================
// I2C Pin Mappings (To avoid overlap with A2/A3)
// ==========================================
#ifdef SIMULATION_MODE
  #define I2C_SDA_PIN 1
  #define I2C_SCL_PIN 2
  #define SIM_ENCODER_CLK 5
  #define SIM_ENCODER_DT  6
  #define SIM_ENCODER_SW  7
  #define MOSFET_PWM_PIN  4

  // Special Calendar Wokwi test value
  // Format: "beach trip,2026-08-02"
  #define SIM_COUNTDOWN "beach trip,2026-08-02"
#endif

#ifndef SIMULATION_MODE
  // STEMMA QT port on Adafruit QT Py ESP32-S3 uses SDA=41, SCL=40
  #define I2C_SDA_PIN 41
  #define I2C_SCL_PIN 40
  // A0 = GPIO 5 (PWM control pin for external MOSFET LCD backlight dimming)
  #define MOSFET_PWM_PIN 5
#endif

// ==========================================
// I2S Audio Pin Mappings
// ==========================================
// Routed to the Adafruit I2S Amplifier BFF (ID 5770)
// A0 = GPIO 18 (DIN/DOUT)
// A1 = GPIO 17 (LRCK)
// A2 = GPIO 9  (BCLK)
#define I2S_DOUT_PIN 18
#define I2S_LRCK_PIN 17
#define I2S_BCLK_PIN 9

// ==========================================
// Alarm Settings
// ==========================================
#define DEFAULT_ALARM_HOUR 7
#define DEFAULT_ALARM_MINUTE 30
#define DEFAULT_ALARM_ENABLED true


// Predefined sound options for the alarm
struct AlarmSound {
    const char* name;
    const char* path;
};

#define NUM_ALARM_SOUNDS 11
static const AlarmSound ALARM_SOUNDS[NUM_ALARM_SOUNDS] = {
    {"Premium Chime",   "/buzzer.wav"},
    {"Harp Triad",      "/chime.wav"},
    {"Zen Bowl",        "/fast_alert.wav"},
    {"Melodic Pulse",   "/siren.wav"},
    {"Soft Alert",      "/chirp.wav"},
    {"Bubble Sparkle",  "/bubble_sparkle.wav"},
    {"Kalimba",         "/kalimba.wav"},
    {"Lofi Beat",       "/lofi_beat.wav"},
    {"Retro Arcade",    "/retro_arcade.wav"},
    {"Dog Bark",        "/dog_bark.mp3"},
    {"Bird Chirps",     "/bird_chirps.mp3"}
};

#endif // CONFIG_H
