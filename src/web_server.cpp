#include "web_server.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "settings_menu.h"
#include "hue_client.h"
#include "spotify_client.h"
#include "audio_controller.h"
#include "school_calendar.h"
#include "display_manager.h"
#include "weather_client.h"
#include "gcal_client.h"
#include "timer_manager.h"

// Instantiate WebServer on port 80
static WebServer server(80);

// Dashboard HTML is now served from SPIFFS (/index.html) to avoid heap allocation.
// To update the dashboard, edit data/index.html and run: pio run --target uploadfs
// --- HTML content removed (was ~74KB inline) ---

// Reject POST bodies that are too large to safely fit in heap
static bool rejectOversizedBody(size_t maxBytes) {
    if (server.hasHeader("Content-Length")) {
        size_t len = server.header("Content-Length").toInt();
        if (len > maxBytes) {
            server.send(413, "text/plain", "Request body too large");
            Serial.printf("[HTTP] Rejected oversized POST: %u bytes (max %u)\r\n", len, maxBytes);
            return true;
        }
    }
    return false;
}

static void sendNoCacheHeaders() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
}

static void handlePlayer();

static String s_lastClientIp = "None";
static String s_lastRequestUri = "None";
static unsigned long s_lastRequestTime = 0;
static uint32_t s_totalRequests = 0;

static void recordClientActivity() {
    s_totalRequests++;
    if (server.client()) {
        s_lastClientIp = server.client().remoteIP().toString();
    }
    s_lastRequestUri = server.uri();
    s_lastRequestTime = millis();
    Serial.printf("[HTTP #%u] %s from %s\r\n", s_totalRequests, s_lastRequestUri.c_str(), s_lastClientIp.c_str());
}

// Serve Dashboard UI from SPIFFS (avoids ~74KB heap allocation)
static void handleRoot() {
    recordClientActivity();
    File file = SPIFFS.open("/index.html", "r");
    if (!file) {
        server.send(500, "text/plain", "Dashboard file missing from SPIFFS. Run: pio run --target uploadfs");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

// Fallback handler for static files stored on SPIFFS
static void handleNotFound() {
    recordClientActivity();
    String path = server.uri();
    if (path.endsWith("/")) path += "index.html";

    // Check with and without .html extension
    String targetPath = path;
    if (!SPIFFS.exists(targetPath) && !targetPath.endsWith(".html")) {
        if (SPIFFS.exists(targetPath + ".html")) {
            targetPath += ".html";
        }
    }

    if (SPIFFS.exists(targetPath)) {
        String contentType = "text/plain";
        if (targetPath.endsWith(".html")) contentType = "text/html";
        else if (targetPath.endsWith(".css")) contentType = "text/css";
        else if (targetPath.endsWith(".js")) contentType = "application/javascript";
        else if (targetPath.endsWith(".png")) contentType = "image/png";
        else if (targetPath.endsWith(".jpg") || targetPath.endsWith(".jpeg")) contentType = "image/jpeg";
        else if (targetPath.endsWith(".ico")) contentType = "image/x-icon";
        else if (targetPath.endsWith(".json")) contentType = "application/json";

        File file = SPIFFS.open(targetPath, "r");
        server.streamFile(file, contentType);
        file.close();
        return;
    }

    server.send(404, "text/plain", "404: Not Found on SPIFFS");
}

// Serve Quick Start Guide from SPIFFS
static void handleQuickStart() {
    recordClientActivity();
    File file = SPIFFS.open("/quick_start_guide.html", "r");
    if (!file) {
        server.send(500, "text/plain", "Quick start guide missing from SPIFFS.");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

// Serve Settings JSON Payload
static void handleGetSettings() {
    recordClientActivity();
    lastWebActivityTime = millis();
    JsonDocument doc;
    
    doc["alarmEnabled"] = alarmEnabled;
    doc["alarmHour"] = alarmHour;
    doc["alarmMinute"] = alarmMinute;
    doc["alarmRepeatMode"] = (int)alarmRepeatMode;
    doc["alarmSoundIndex"] = alarmSoundIndex;
    doc["alarmVolume"] = alarmVolume;
    doc["gentleWakeEnabled"] = gentleWakeEnabled;
    doc["currentVolume"] = currentVolume;
    doc["spotifyVolumePercent"] = getSpotifyVolumePercent();
    doc["schoolCalendarEnabled"] = schoolCalendarEnabled;
    
    doc["hueLightsEnabled"] = hueLightsEnabled;
    doc["hueBrightness"] = hueBrightness;
    doc["hueSunriseEnabled"] = hueSunriseEnabled;
    doc["hueSunriseDuration"] = hueSunriseDuration;
    doc["hueBridgeIP"] = hueBridgeIP;
    doc["activeHueSceneID"] = activeHueSceneID;

    if (wmataApiKey.length() > 0) {
        doc["wmataApiKey"] = "********";
    } else {
        doc["wmataApiKey"] = "";
    }
    doc["wmataStopId"] = wmataStopId;
    doc["wmataRouteFilter"] = wmataRouteFilter;
    doc["wmataMode"] = (int)wmataMode;
    doc["nightBigClock"] = nightBigClock;

    GCalSnapshot gSnap = getGCalSnapshot();
    doc["gcalEnabled"] = gSnap.enabled;
    doc["gcalUrl"] = getGCalUrl();
    doc["gcalFilterKeyword"] = gSnap.filterKeyword;
    doc["gcalLookahead"] = gSnap.lookaheadHours;
    doc["gcalLoaded"] = gSnap.loaded;
    doc["gcalEventCount"] = gSnap.eventCount;
    doc["gcalNextEvent"] = gSnap.nextEvent.displayString;

    TimerSnapshot tSnap = getTimerSnapshot();
    doc["timerActive"] = tSnap.active;
    doc["timerRemainingSeconds"] = tSnap.remainingSeconds;
    doc["timerTotalSeconds"] = tSnap.totalSeconds;
    doc["timerTimeString"] = tSnap.timeString;
    doc["timerProgress"] = tSnap.progress;

    // Mask sensitive Spotify credentials
    String maskedClientId = "";
    if (strlen(SPOTIFY_CLIENT_ID) > 8) {
        maskedClientId = String(SPOTIFY_CLIENT_ID).substring(0, 4) + "..." + String(SPOTIFY_CLIENT_ID).substring(strlen(SPOTIFY_CLIENT_ID) - 4);
    } else {
        maskedClientId = "********";
    }
    doc["spotifyClientId"] = maskedClientId;
    doc["spotifyClientSecret"] = "********";

    JsonArray scenesArr = doc["hueScenes"].to<JsonArray>();
    for (int i = 0; i < numHueScenes; i++) {
        JsonObject s = scenesArr.add<JsonObject>();
        s["id"] = hueScenes[i].id;
        s["name"] = hueScenes[i].name;
    }
    
    doc["backlightState"] = backlightState;
    doc["weatherEnabled"] = weatherEnabled;
    doc["weatherFetched"] = weatherFetched;
    doc["weatherTemp"] = weatherTemp;
    doc["weatherLabel"] = weatherLabel;
    doc["weatherHighTemp"] = weatherHighTemp;
    doc["weatherFeelsLike"] = weatherFeelsLike;
    doc["weatherPrecipitationChance"] = weatherPrecipitationChance;
    doc["weatherPrecipitationHours"] = weatherPrecipitationHours;
    doc["weatherRainStartHour"] = weatherRainStartHour;
    doc["weatherUVIndex"] = weatherUVIndex;
    doc["weatherUVLabel"] = weatherUVLabel;
    doc["weatherTomorrowHighTemp"] = weatherTomorrowHighTemp;
    doc["weatherTomorrowFeelsLike"] = weatherTomorrowFeelsLike;
    doc["weatherTomorrowUVIndex"] = weatherTomorrowUVIndex;
    doc["weatherTomorrowUVLabel"] = weatherTomorrowUVLabel;
    doc["weatherTomorrowPrecipChance"] = weatherTomorrowPrecipChance;
    doc["weatherTomorrowPrecipitationHours"] = weatherTomorrowPrecipitationHours;
    doc["weatherTomorrowRainStartHour"] = weatherTomorrowRainStartHour;
    doc["weatherAlertActive"] = weatherAlertActive;
    doc["weatherAlertTitle"] = weatherAlertTitle;
    doc["autoDimmingEnabled"] = autoDimmingEnabled;
    doc["nightBrightness"] = nightBrightness;
    doc["dayBrightness"] = dayBrightness;
    doc["firmwareVersion"] = FIRMWARE_VERSION;
    doc["uptimeSeconds"] = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    doc["sleepDurationMinutes"] = sleepDurationMinutes;
    doc["spotifyShuffleState"] = getSpotifyShuffleState();
    doc["spotifyVolumePercent"] = getSpotifyVolumePercent();
    
    doc["reminderEnabled"] = reminderEnabled;
    doc["reminderHour"] = reminderHour;
    doc["reminderMinute"] = reminderMinute;
    doc["reminderText"] = reminderText;
    doc["reminderSound"] = reminderSound;

    JsonArray countdownArr = doc["countdowns"].to<JsonArray>();
    for (int i = 0; i < MAX_COUNTDOWN_EVENTS; i++) {
        JsonObject c = countdownArr.add<JsonObject>();
        c["enabled"] = countdownEvents[i].enabled;
        c["name"] = countdownEvents[i].name;
        c["year"] = countdownEvents[i].year;
        c["month"] = countdownEvents[i].month;
        c["day"] = countdownEvents[i].day;
    }
    doc["maxCountdownEvents"] = MAX_COUNTDOWN_EVENTS;

    JsonArray affArr = doc["affirmations"].to<JsonArray>();
    for (int i = 0; i < 15; i++) {
        String aff = affirmations[i];
        aff.replace("\x07", "♥");
        affArr.add(aff);
    }

    JsonArray plArr = doc["playlists"].to<JsonArray>();
    int plCount = getSpotifyNumPlaylists();
    for (int i = 0; i < plCount; i++) {
        JsonObject p = plArr.add<JsonObject>();
        p["name"] = getSpotifyPlaylistName(i);
        p["uri"] = getSpotifyPlaylistUri(i);
    }

    JsonArray allPlArr = doc["allPlaylists"].to<JsonArray>();
    int allPlCount = getAllSpotifyNumPlaylists();
    for (int i = 0; i < allPlCount; i++) {
        JsonObject p = allPlArr.add<JsonObject>();
        p["name"] = getAllSpotifyPlaylistName(i);
        p["uri"] = getAllSpotifyPlaylistUri(i);
    }

    JsonArray favPlArr = doc["favoritePlaylists"].to<JsonArray>();
    for (int i = 0; i < MAX_SPOTIFY_PLAYLISTS; i++) {
        favPlArr.add(favoritePlaylistUris[i]);
    }

    JsonArray devArr = doc["spotifyDevices"].to<JsonArray>();
    int devCount = getSpotifyNumDevices();
    for (int i = 0; i < devCount; i++) {
        JsonObject d = devArr.add<JsonObject>();
        d["name"] = getSpotifyDeviceName(i);
        d["id"] = getSpotifyDeviceId(i);
    }
    doc["selectedSpotifyDeviceIndex"] = getSpotifySelectedDeviceIndex();

    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
    doc["wifiRssi"] = rssi;
    doc["ipAddress"] = WiFi.localIP().toString();

    int quality = 0;
    if (rssi <= -100) quality = 0;
    else if (rssi >= -50) quality = 100;
    else quality = 2 * (rssi + 100);

    const char* signalLabel = "📶 Poor";
    if (rssi >= -50) signalLabel = "📶 Excellent";
    else if (rssi >= -65) signalLabel = "📶 Good";
    else if (rssi >= -75) signalLabel = "📶 Fair";
    else if (rssi >= -85) signalLabel = "📶 Weak";

    char wifiBuf[64];
    snprintf(wifiBuf, sizeof(wifiBuf), "%s (%d%%)", signalLabel, quality);
    doc["wifiSignalText"] = String(wifiBuf);

    struct tm timeinfo;
    char timeStr[32] = "--:-- --";
    if (getLocalTime(&timeinfo, 0)) {
        int displayHour = timeinfo.tm_hour;
        const char* ampm = (displayHour >= 12) ? "PM" : "AM";
        if (displayHour > 12) displayHour -= 12;
        if (displayHour == 0) displayHour = 12;
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d %s", displayHour, timeinfo.tm_min, ampm);
    }
    doc["clockTime"] = timeStr;

    String stateStr = "Idle Clock";
    if (weatherAlertActive && !weatherAlertTitle.isEmpty()) {
        stateStr = "⚠️ ALERT: " + weatherAlertTitle;
    } else if (currentState == STATE_ALARM_RINGING) {
        stateStr = "🔔 ALARM RINGING!";
    } else if (currentState == STATE_ALERT_MESSAGE) {
        stateStr = "📢 " + intercomMessage;
    } else if (currentState == STATE_NOISE_MACHINE) {
        stateStr = "🌙 Sleep Noise Active";
    } else if (currentState == STATE_SUNRISE_CANCEL_PROMPT) {
        stateStr = "🌅 Sunrise Light Active";
    } else if (currentState == STATE_SETTINGS_MENU) {
        stateStr = "⚙️ In Clock Menu";
    } else if (currentState == STATE_MUSIC_MODE) {
        stateStr = "🎵 Spotify Active";
    }
    doc["clockState"] = stateStr;
    
    bool serialConnected = (bool)Serial;
    doc["powerSource"] = serialConnected ? "💻 USB Computer (Mac)" : "⚡ Power Bank / Wall AC";
    
    esp_reset_reason_t rstReason = esp_reset_reason();
    const char* rstStr = "Power-On";
    switch (rstReason) {
        case ESP_RST_POWERON:   rstStr = "Power-On"; break;
        case ESP_RST_SW:        rstStr = "Software Restart"; break;
        case ESP_RST_PANIC:     rstStr = "Software Panic / Crash"; break;
        case ESP_RST_INT_WDT:   rstStr = "Interrupt Watchdog"; break;
        case ESP_RST_TASK_WDT:  rstStr = "Task Watchdog"; break;
        case ESP_RST_WDT:       rstStr = "Other Watchdog"; break;
        case ESP_RST_DEEPSLEEP: rstStr = "Deep Sleep Wake"; break;
        case ESP_RST_BROWNOUT:  rstStr = "Brownout (Voltage Sag)"; break;
        case ESP_RST_SDIO:      rstStr = "SDIO Reset"; break;
        default:                rstStr = "Normal / Unknown"; break;
    }
    doc["resetReason"] = rstStr;
    
    String response;
    serializeJson(doc, response);
    sendNoCacheHeaders();
    server.send(200, "application/json", response);
}

// Save Settings from POST body
static void handlePostSettings() {
    lastWebActivityTime = millis();
    if (rejectOversizedBody(4096)) return;  // Settings JSON should be < 4KB
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server.arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }
    
    // Parse settings and update active variables
    if (!doc["alarmEnabled"].isNull()) alarmEnabled = doc["alarmEnabled"];
    if (!doc["alarmHour"].isNull()) alarmHour = doc["alarmHour"];
    if (!doc["alarmMinute"].isNull()) alarmMinute = doc["alarmMinute"];
    if (!doc["alarmRepeatMode"].isNull()) alarmRepeatMode = (AlarmRepeatMode)doc["alarmRepeatMode"].as<int>();
    if (!doc["alarmSoundIndex"].isNull()) alarmSoundIndex = doc["alarmSoundIndex"];
    if (!doc["alarmVolume"].isNull()) alarmVolume = doc["alarmVolume"];
    if (!doc["gentleWakeEnabled"].isNull()) gentleWakeEnabled = doc["gentleWakeEnabled"];
    
    if (!doc["hueLightsEnabled"].isNull()) hueLightsEnabled = doc["hueLightsEnabled"];
    if (!doc["hueBrightness"].isNull()) hueBrightness = doc["hueBrightness"];
    if (!doc["hueSunriseEnabled"].isNull()) hueSunriseEnabled = doc["hueSunriseEnabled"];
    if (!doc["hueSunriseDuration"].isNull()) hueSunriseDuration = doc["hueSunriseDuration"];
    if (!doc["hueBridgeIP"].isNull()) {
        hueBridgeIP = doc["hueBridgeIP"].as<String>();
    }
    if (!doc["activeHueSceneID"].isNull()) {
        activeHueSceneID = doc["activeHueSceneID"].as<String>();
        for (int i = 0; i < numHueScenes; i++) {
            if (hueScenes[i].id == activeHueSceneID) {
                activeHueSceneName = hueScenes[i].name;
                break;
            }
        }
    }

    if (!doc["sleepDurationMinutes"].isNull()) {
        unsigned long newSleep = doc["sleepDurationMinutes"].as<unsigned long>();
        sleepDurationMinutes = newSleep;
        if (sleepDurationMinutes > 0) {
            sleepTimerEnd = millis() + (sleepDurationMinutes * 60 * 1000L);
            if (hueLightsEnabled) {
                startHueSleepTimerFade(sleepDurationMinutes);
            }
            Serial.printf("[Web] Sleep timer armed for %lu minutes.\r\n", sleepDurationMinutes);
        } else {
            sleepTimerEnd = 0;
        }
    }
    
    if (!doc["currentVolume"].isNull()) {
        int vol = doc["currentVolume"].as<int>();
        if (vol >= 0 && vol <= 21 && vol != currentVolume) {
            currentVolume = vol;
            isMuted = false;
            updateAudioVolume();
            volumeDisplayTimeout = millis() + 2000;
            lastLCDUpdate = 0;
            pendingSaveSettingsTime = millis() + 2000; // Debounce NVS save
        }
    }
    
    if (!doc["spotifyVolumePercent"].isNull()) {
        int pct = doc["spotifyVolumePercent"].as<int>();
        if (pct >= 0 && pct <= 100) {
            setSpotifyVolumePercent(pct);
            setPendingSpotifyVolume(pct);
            volumeDisplayTimeout = millis() + 2000;
            lastLCDUpdate = 0;
        }
    }
    if (!doc["spotifyShuffleState"].isNull()) {
        SpotifyCommand cmd = {SPOTIFY_CMD_SET_SHUFFLE, 0, doc["spotifyShuffleState"].as<bool>(), ""};
        postSpotifyCommand(cmd);
    }
    
    if (!doc["backlightState"].isNull()) {
        backlightState = doc["backlightState"];
        setBacklight(backlightState);
    }
    if (!doc["weatherEnabled"].isNull()) weatherEnabled = doc["weatherEnabled"];
    if (!doc["schoolCalendarEnabled"].isNull()) schoolCalendarEnabled = doc["schoolCalendarEnabled"];
    if (!doc["autoDimmingEnabled"].isNull()) autoDimmingEnabled = doc["autoDimmingEnabled"];
    if (!doc["nightBrightness"].isNull()) nightBrightness = doc["nightBrightness"];
    if (!doc["dayBrightness"].isNull()) dayBrightness = doc["dayBrightness"];
    updateHardwareBrightness();

    if (!doc["reminderEnabled"].isNull()) reminderEnabled = doc["reminderEnabled"];
    if (!doc["reminderHour"].isNull()) reminderHour = doc["reminderHour"];
    if (!doc["reminderMinute"].isNull()) reminderMinute = doc["reminderMinute"];
    if (!doc["reminderText"].isNull()) reminderText = doc["reminderText"].as<String>();
    if (!doc["reminderSound"].isNull()) reminderSound = doc["reminderSound"].as<String>();

    if (!doc["countdowns"].isNull()) {
        JsonArray cntArr = doc["countdowns"].as<JsonArray>();
        int i = 0;
        for (JsonObject c : cntArr) {
            if (i >= MAX_COUNTDOWN_EVENTS) break;
            if (!c["enabled"].isNull()) countdownEvents[i].enabled = c["enabled"];
            if (!c["name"].isNull()) countdownEvents[i].name = c["name"].as<String>();
            if (!c["year"].isNull()) countdownEvents[i].year = c["year"];
            if (!c["month"].isNull()) countdownEvents[i].month = c["month"];
            if (!c["day"].isNull()) countdownEvents[i].day = c["day"];
            i++;
        }
        for (; i < MAX_COUNTDOWN_EVENTS; i++) {
            countdownEvents[i].enabled = false;
            countdownEvents[i].name = "";
            countdownEvents[i].year = 0;
            countdownEvents[i].month = 0;
            countdownEvents[i].day = 0;
        }
    }

    if (!doc["wmataApiKey"].isNull()) {
        String newKey = doc["wmataApiKey"].as<String>();
        if (newKey.length() > 0 && !newKey.startsWith("****") && newKey != "********") {
            wmataApiKey = newKey;
        }
    }
    if (!doc["wmataStopId"].isNull()) wmataStopId = doc["wmataStopId"].as<String>();
    if (!doc["wmataRouteFilter"].isNull()) wmataRouteFilter = doc["wmataRouteFilter"].as<String>();
    if (!doc["wmataMode"].isNull()) wmataMode = (WmataMode)doc["wmataMode"].as<int>();
    if (!doc["nightBigClock"].isNull()) {
        nightBigClock = doc["nightBigClock"].as<int>();
        if (nightBigClock < 0 || nightBigClock > 2) nightBigClock = 2;
    }

    if (!doc["gcalEnabled"].isNull()) setGCalEnabled(doc["gcalEnabled"].as<bool>());
    if (!doc["gcalUrl"].isNull()) setGCalUrl(doc["gcalUrl"].as<String>());
    if (!doc["gcalFilterKeyword"].isNull()) setGCalFilterKeyword(doc["gcalFilterKeyword"].as<String>());
    if (!doc["gcalLookahead"].isNull()) setGCalLookaheadHours(doc["gcalLookahead"].as<int>());

    JsonArray affArr = doc["affirmations"].as<JsonArray>();
    if (!affArr.isNull()) {
        int i = 0;
        for (JsonVariant v : affArr) {
            if (i < 15) {
                String t = v.as<String>();
                t.trim();
                if (t.length() > 16) t = t.substring(0, 16);
                affirmations[i] = t;
                i++;
            }
        }
    }

    // Persist modifications to NVS
    saveSettings();
    
    // Apply Hue bulb adjustments in real time
    updateHueLightState();
    
    server.send(200, "text/plain", "OK");
    lastLCDUpdate = 0; // Force update to display
}

static void handleTestAlarm() {
    Serial.println("[Web] Test Alarm & Sunrise Light triggered from Web App!");
    startAlarmPlayback();
    currentState = STATE_ALARM_RINGING;
    if (hueSunriseEnabled) {
        triggerHueSunriseTransitionAsync();
    }
    server.send(200, "text/plain", "OK");
    lastLCDUpdate = 0;
}

static void handleGoodNight() {
    Serial.println("[Web] Good Night bedtime preset activated!");
    stopAudioPlayback();
    startAudioPlayback("/brown_noise.wav", true);
    currentState = STATE_NOISE_MACHINE;
    sleepDurationMinutes = 30;
    sleepTimerEnd = millis() + (30 * 60 * 1000);
    
    if (hueLightsEnabled) {
        startHueSleepTimerFade(30);
    }
    
    server.send(200, "text/plain", "OK");
    lastLCDUpdate = 0;
}

// TEMP: stream back the recorded voice memo so the Web UI can play it in the browser
static void handleGetVoiceMemo() {
    File f = SPIFFS.open("/voice_memo.wav", "r");
    if (!f || f.size() < 44) {
        server.send(404, "text/plain", "No voice memo recorded");
        return;
    }
    server.streamFile(f, "audio/wav");
    f.close();
}

// Spotify Remote endpoints
static void handleSpotifyPlay() {
    SpotifyCommand cmd = {SPOTIFY_CMD_PLAY, 0, false, ""};
    postSpotifyCommand(cmd);
    server.send(200, "text/plain", "OK");
}

static void handleSpotifyPause() {
    SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
    postSpotifyCommand(cmd);
    server.send(200, "text/plain", "OK");
}

static void handleSpotifyNext() {
    SpotifyCommand cmd = {SPOTIFY_CMD_NEXT, 0, false, ""};
    postSpotifyCommand(cmd);
    server.send(200, "text/plain", "OK");
}

static void handleGetSpotifyTrack() {
    String text = getSpotifyTrackText();
    if (getSpotifyIsPlaying()) {
        text = "▶ " + text;
    } else {
        text = "⏸ [PAUSED] " + text;
    }
    sendNoCacheHeaders();
    server.send(200, "text/plain", text);
}

static void handleSpotifyExit() {
    Serial.println("[Spotify] Exit Music Mode requested via Web Dashboard.");
    SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
    postSpotifyCommand(cmd);
    setSpotifyIsPlaying(false);
    currentState = STATE_IDLE_CLOCK;
    spotifyPausedStartTime = 0;
    lastLCDUpdate = 0;
    server.send(200, "text/plain", "OK");
}

static void handleSpotifyVolume() {
    if (server.hasArg("percent")) {
        int pct = server.arg("percent").toInt();
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        
        setSpotifyVolumePercent(pct);
        setPendingSpotifyVolume(pct);
        
        volumeDisplayTimeout = millis() + 2000;
        lastLCDUpdate = 0;
        
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Missing percent");
    }
}

static void handleSpotifyPlayPlaylist() {
    if (server.hasArg("uri")) {
        String uri = server.arg("uri");
        Serial.printf("[Spotify] Play playlist via Web: %s\r\n", uri.c_str());
        SpotifyCommand cmd = {SPOTIFY_CMD_PLAY_PLAYLIST, 0, false, ""};
        strncpy(cmd.stringParam, uri.c_str(), sizeof(cmd.stringParam) - 1);
        postSpotifyCommand(cmd);
        if (getSpotifyShuffleState()) {
            SpotifyCommand shufCmd = {SPOTIFY_CMD_SET_SHUFFLE, 0, true, ""};
            postSpotifyCommand(shufCmd);
        }
        currentState = STATE_MUSIC_MODE;
        server.send(200, "text/plain", "OK");
        lastLCDUpdate = 0;
    } else {
        server.send(400, "text/plain", "Missing uri");
    }
}

static void handleSpotifySelectDevice() {
    if (server.hasArg("id")) {
        String devId = server.arg("id");
        Serial.printf("[Spotify] Select device via Web: %s\r\n", devId.c_str());
        SpotifyCommand cmd = {SPOTIFY_CMD_SELECT_DEVICE, 0, false, ""};
        strncpy(cmd.stringParam, devId.c_str(), sizeof(cmd.stringParam) - 1);
        postSpotifyCommand(cmd);
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Missing id");
    }
}

static void handleSpotifyFetchDevices() {
    SpotifyCommand cmdDev = {SPOTIFY_CMD_FETCH_DEVICES, 0, false, ""};
    postSpotifyCommand(cmdDev);
    SpotifyCommand cmdPl = {SPOTIFY_CMD_FETCH_PLAYLISTS, 0, false, ""};
    postSpotifyCommand(cmdPl);
    server.send(200, "text/plain", "OK");
}

static void handleSpotifySaveFavorites() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    JsonArray favs = doc["favorites"].as<JsonArray>();
    String newFavs[MAX_SPOTIFY_PLAYLISTS];
    int idx = 0;
    for (JsonVariant v : favs) {
        if (idx >= MAX_SPOTIFY_PLAYLISTS) break;
        newFavs[idx++] = v.as<String>();
    }
    while (idx < MAX_SPOTIFY_PLAYLISTS) {
        newFavs[idx++] = "";
    }
    saveFavoritePlaylists(newFavs);

    SpotifyCommand cmd = {SPOTIFY_CMD_FETCH_PLAYLISTS, 0, false, ""};
    postSpotifyCommand(cmd);

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// Audio Engine controls
static void handleAudioNoiseOn() {
    currentState = STATE_NOISE_MACHINE;
    startAudioPlayback("/brown_noise.wav", true);
    server.send(200, "text/plain", "OK");
    lastLCDUpdate = 0;
}

static void handleAudioNoiseOff() {
    stopAudioPlayback();
    setSpotifyIsPlaying(false);
    currentState = STATE_IDLE_CLOCK;
    server.send(200, "text/plain", "OK");
    lastLCDUpdate = 0;
}

static void handlePostMessage() {
    if (rejectOversizedBody(1024)) return;  // Text messages should be < 1KB
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Bad Request");
        return;
    }
    
    String body = server.arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }
    
    if (!doc["text"].isNull()) {
        intercomMessage = doc["text"].as<String>();
        alertHeader = "  PARENT ALERT \x05";
        currentState = STATE_ALERT_MESSAGE;
        
        String sound = "silent";
        if (!doc["sound"].isNull()) {
            sound = doc["sound"].as<String>();
        }
        
        stopAudioPlayback();
        playNamedSound(sound);
        
        lastLCDUpdate = 0;
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Missing text field");
    }
}

static void handleGetSchoolCalendar() {
    JsonDocument doc;
    JsonArray daysArr = doc.to<JsonArray>();
    
    struct tm timeinfo;
    int currentYear = 2026;
    int currentMonth = 1;
    int currentDay = 1;
    
    if (getLocalTime(&timeinfo, 0)) {
        currentYear = timeinfo.tm_year + 1900;
        currentMonth = timeinfo.tm_mon + 1;
        currentDay = timeinfo.tm_mday;
    }
    
    for (int i = 0; i < numSchoolDaysOff; i++) {
        if (schoolDaysOff[i].year > currentYear ||
            (schoolDaysOff[i].year == currentYear && schoolDaysOff[i].month > currentMonth) ||
            (schoolDaysOff[i].year == currentYear && schoolDaysOff[i].month == currentMonth && schoolDaysOff[i].day >= currentDay)) {
            
            JsonObject obj = daysArr.add<JsonObject>();
            char dateBuf[12];
            snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", schoolDaysOff[i].year, schoolDaysOff[i].month, schoolDaysOff[i].day);
            obj["date"] = String(dateBuf);
            obj["name"] = schoolDaysOff[i].description;
        }
    }
    
    String response;
    serializeJson(doc, response);
    sendNoCacheHeaders();
    server.send(200, "application/json", response);
}

static void handlePostWeatherAlert() {
    if (rejectOversizedBody(512)) return;  // Alert payloads should be < 512B
    if (server.hasArg("plain")) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (!err) {
            if (!doc["active"].isNull()) weatherAlertActive = doc["active"].as<bool>();
            if (!doc["title"].isNull()) weatherAlertTitle = doc["title"].as<String>();
            lastLCDUpdate = 0;
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    // Toggle test alert if no JSON provided
    weatherAlertActive = !weatherAlertActive;
    if (weatherAlertActive) weatherAlertTitle = "Svr T-Stm Warning";
    else weatherAlertTitle = "";
    lastLCDUpdate = 0;
    server.send(200, "text/plain", "OK");
}

static void handlePostWeatherSync() {
    lastWebActivityTime = millis();
    triggerImmediateWeatherSync();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Weather sync triggered\"}");
}

static void handlePostGCalSync() {
    lastWebActivityTime = millis();
    triggerImmediateGCalSync();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Google Calendar sync triggered\"}");
}

static File voiceUploadFile;
static const size_t MAX_VOICE_UPLOAD_SIZE = 500000;  // 15s of 16kHz 16-bit mono WAV + header

static void handleVoiceUploadChunk() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        stopAudioPlayback(); // Ensure audio engine is completely idle before writing to SPIFFS
        voiceUploadFile = SPIFFS.open("/voice_memo.wav", FILE_WRITE);
        Serial.printf("[Voice] Chunk upload started: %s\r\n", upload.filename.c_str());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (upload.totalSize + upload.currentSize > MAX_VOICE_UPLOAD_SIZE) {
            Serial.printf("[Voice] Upload too large (%u bytes), aborting.\r\n", upload.totalSize + upload.currentSize);
            if (voiceUploadFile) { voiceUploadFile.close(); }
            return;
        }
        if (voiceUploadFile) {
            voiceUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (voiceUploadFile) {
            voiceUploadFile.close();
            Serial.printf("[Voice] Chunk upload complete: %u total bytes\r\n", upload.totalSize);
        }
    }
}

static File fsUploadFile;

static void handleUploadFileChunk() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        stopAudioPlayback();
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        fsUploadFile = SPIFFS.open(filename, FILE_WRITE);
        Serial.printf("[FS] File upload started: %s\r\n", filename.c_str());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            fsUploadFile.close();
            Serial.printf("[FS] File upload complete: %u bytes\r\n", upload.totalSize);
        }
    }
}

static void handlePostVoiceIntercom() {
    intercomMessage = "Parent Voice Memo";
    alertHeader = "  VOICE ALERT \x05";
    currentState = STATE_ALERT_MESSAGE;
    
    stopAudioPlayback();
    startAudioPlayback("/voice_memo.wav");
    lastLCDUpdate = 0;
    
    server.send(200, "text/plain", "OK");
}

static void handleUnlockSettings() {
    if (rejectOversizedBody(512)) return;
    if (server.hasArg("plain")) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (!err && !doc["passcode"].isNull()) {
            String inputPasscode = doc["passcode"].as<String>();
            if (inputPasscode == SECRET_SETTINGS_PASSCODE) {
                server.send(200, "application/json", "{\"success\":true}");
                return;
            }
        }
    }
    server.send(403, "application/json", "{\"success\":false}");
}

static const char PROGMEM PLAYER_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Spotify Clock Remote</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--accent:#1db954;--text:#f8fafc;--sub:#94a3b8;--border:rgba(255,255,255,0.1);}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;}
body{background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:1rem;}
.card{background:var(--card);border:1px solid var(--border);border-radius:24px;padding:1.6rem;width:100%;max-width:360px;box-shadow:0 20px 40px rgba(0,0,0,0.5);text-align:center;}
.badge{display:inline-block;padding:0.3rem 0.85rem;border-radius:999px;font-size:0.75rem;font-weight:700;text-transform:uppercase;margin-bottom:1rem;}
.badge-playing{background:rgba(29,185,84,0.2);color:var(--accent);border:1px solid var(--accent);}
.badge-paused{background:rgba(148,163,184,0.2);color:var(--sub);border:1px solid var(--border);}
.icon{font-size:3rem;margin-bottom:0.4rem;}
.track{font-size:1.15rem;font-weight:700;margin-bottom:0.25rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.status{font-size:0.85rem;color:var(--sub);margin-bottom:1.3rem;}
.btn-row{display:flex;gap:0.85rem;justify-content:center;align-items:center;margin-bottom:1.4rem;}
button{border:none;cursor:pointer;border-radius:14px;font-weight:700;transition:transform 0.1s,opacity 0.2s;}
button:active{transform:scale(0.95);}
.btn-ctrl{width:56px;height:56px;font-size:1.3rem;display:flex;align-items:center;justify-content:center;background:#334155;color:white;}
.btn-play{background:var(--accent);color:#0f172a;width:68px;height:68px;font-size:1.7rem;border-radius:50%;display:flex;align-items:center;justify-content:center;}
.btn-pause{background:#ef4444;color:white;width:68px;height:68px;font-size:1.7rem;border-radius:50%;display:flex;align-items:center;justify-content:center;}
.vol-box{background:rgba(0,0,0,0.2);padding:0.9rem 1.1rem;border-radius:14px;border:1px solid var(--border);margin-bottom:1.2rem;}
.vol-header{display:flex;justify-content:space-between;font-size:0.85rem;color:var(--sub);margin-bottom:0.5rem;}
input[type=range]{width:100%;accent-color:var(--accent);height:6px;}
.playlists{display:flex;flex-direction:column;gap:0.5rem;text-align:left;}
.pl-btn{background:#334155;color:white;padding:0.65rem 0.9rem;font-size:0.82rem;border-radius:10px;display:flex;align-items:center;gap:0.5rem;width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.nav-link{display:inline-block;margin-top:1.2rem;color:var(--sub);font-size:0.8rem;text-decoration:none;}
.nav-link:hover{color:var(--text);}
</style>
</head>
<body>
<div class="card">
  <span id="badge" class="badge badge-paused">Checking...</span>
  <div class="icon">🎵</div>
  <div id="track" class="track">Loading...</div>
  <div id="device" class="status">Faye's Echo Dot</div>
  
  <div class="btn-row">
    <button class="btn-ctrl" onclick="sendCmd('prev')">⏮️</button>
    <button id="toggleBtn" class="btn-play" onclick="togglePlay()">▶️</button>
    <button class="btn-ctrl" onclick="sendCmd('next')">⏭️</button>
  </div>
  
  <div class="vol-box">
    <div class="vol-header">
      <span>🔊 Speaker Volume</span>
      <span id="volNum">--%</span>
    </div>
    <input type="range" id="volSlider" min="0" max="100" value="20" oninput="document.getElementById('volNum').textContent=this.value+'%'" onchange="sendVol(this.value)">
  </div>
  
  <div class="playlists">
    <span style="font-size:0.75rem;color:var(--sub);font-weight:700;text-transform:uppercase;">Quick Playlists</span>
    <button class="pl-btn" onclick="playPl(0)">▶️ The best music playlist</button>
    <button class="pl-btn" onclick="playPl(1)">▶️ Building the band bands</button>
    <button class="pl-btn" onclick="playPl(2)">▶️ Bedtime Meditations</button>
  </div>

  <div class="vol-box" style="margin-top:0.8rem;">
    <div class="vol-header">
      <span>🌙 Sleep Timer</span>
      <span id="sleepTimerBadge" style="color:var(--accent);font-weight:700;">Off</span>
    </div>
    <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:0.4rem;margin-top:0.4rem;">
      <button class="pl-btn" style="justify-content:center;padding:0.55rem 0;" onclick="setSleep(15)">15m</button>
      <button class="pl-btn" style="justify-content:center;padding:0.55rem 0;" onclick="setSleep(30)">30m</button>
      <button class="pl-btn" style="justify-content:center;padding:0.55rem 0;" onclick="setSleep(45)">45m</button>
      <button class="pl-btn" style="justify-content:center;padding:0.55rem 0;background:#475569;" onclick="setSleep(0)">Off</button>
    </div>
  </div>

  <a href="/" class="nav-link">⚙️ Clock Settings Dashboard →</a>
</div>

<script>
let isPlaying = false;
const uris = [
  'spotify:playlist:5ZJDTXgElYe0NjQQTj4Pkj',
  'spotify:playlist:4bDg2zYRQf4M6dsnaH2s3L',
  'spotify:playlist:3XHMabF1dqaCrbHJYzqoCA'
];
async function update() {
  try {
    const r = await fetch('/api/spotify/debug');
    if (r.ok) {
      const d = await r.json();
      isPlaying = d.isPlaying;
      document.getElementById('track').textContent = d.track || 'Idle';
      const b = document.getElementById('badge');
      const btn = document.getElementById('toggleBtn');
      if (isPlaying) {
        b.className = 'badge badge-playing';
        b.textContent = '▶ PLAYING';
        btn.className = 'btn-pause';
        btn.innerHTML = '⏸️';
      } else {
        b.className = 'badge badge-paused';
        b.textContent = '⏸ PAUSED';
        btn.className = 'btn-play';
        btn.innerHTML = '▶️';
      }
      const s = document.getElementById('volSlider');
      if (document.activeElement !== s) {
        s.value = d.volumePercent;
        document.getElementById('volNum').textContent = d.volumePercent + '%';
      }
      const sm = d.sleepRemainingMinutes || 0;
      document.getElementById('sleepTimerBadge').textContent = (sm > 0) ? (sm + 'm left') : 'Off';
    }
  } catch(e){}
}
async function togglePlay() {
  const ep = isPlaying ? '/api/spotify/pause' : '/api/spotify/play';
  await fetch(ep, {method:'POST'});
  setTimeout(update, 400);
}
async function sendCmd(action) {
  if (action === 'next') await fetch('/api/spotify/next', {method:'POST'});
  if (action === 'prev') await fetch('/api/spotify/prev', {method:'POST'});
  setTimeout(update, 600);
}
async function sendVol(val) {
  await fetch('/api/spotify/volume', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'percent=' + val
  });
}
async function setSleep(mins) {
  await fetch('/api/timer/sleep', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({minutes: mins})
  });
  setTimeout(update, 400);
}
async function playPl(idx) {
  if (idx < uris.length) {
    await fetch('/api/spotify/play_playlist', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'uri=' + encodeURIComponent(uris[idx])
    });
    setTimeout(update, 1500);
  }
}
update();
setInterval(update, 3000);
</script>
</body>
</html>
)rawliteral";

static void handlePlayer() {
    recordClientActivity();
    lastWebActivityTime = millis();
    server.send_P(200, "text/html", PLAYER_HTML);
}

void setupWebServer() {
    // Start mDNS Responder for http://spotify-alarm.local
    if (MDNS.begin("spotify-alarm")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] Responder started: http://spotify-alarm.local");
    }

    // Collect Content-Length header for POST body size validation
    const char* headerKeys[] = {"Content-Length"};
    server.collectHeaders(headerKeys, 1);

    // Define web route handlers
    server.on("/", HTTP_GET, handleRoot);
    server.on("/player", HTTP_GET, handlePlayer);
    server.on("/spotify", HTTP_GET, handlePlayer);
    server.on("/quick_start_guide.html", HTTP_GET, handleQuickStart);
    server.on("/quick_start_guide", HTTP_GET, handleQuickStart);
    server.on("/guide", HTTP_GET, handleQuickStart);
    server.onNotFound(handleNotFound);
    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/settings", HTTP_POST, handlePostSettings);
    server.on("/api/unlock_settings", HTTP_POST, handleUnlockSettings);
    server.on("/api/message", HTTP_POST, handlePostMessage);
    server.on("/api/weather_alert", HTTP_POST, handlePostWeatherAlert);
    server.on("/api/weather_sync", HTTP_POST, handlePostWeatherSync);
    server.on("/api/gcal_sync", HTTP_POST, handlePostGCalSync);
    
    // New Web & Presets Endpoints
    server.on("/api/test_alarm", HTTP_POST, handleTestAlarm);
    server.on("/api/good_night", HTTP_POST, handleGoodNight);
    server.on("/api/voice_intercom", HTTP_POST, handlePostVoiceIntercom, handleVoiceUploadChunk);
    server.on("/api/voice_memo.wav", HTTP_GET, handleGetVoiceMemo);
    server.on("/api/upload_file", HTTP_POST, []() {
        server.send(200, "text/plain", "OK");
    }, handleUploadFileChunk);

    // Spotify playback remote endpoints
    server.on("/api/spotify/play_playlist", HTTP_POST, handleSpotifyPlayPlaylist);
    server.on("/api/spotify/select_device", HTTP_POST, handleSpotifySelectDevice);
    server.on("/api/spotify/fetch_devices", HTTP_POST, handleSpotifyFetchDevices);
    server.on("/api/spotify/save_favorites", HTTP_POST, handleSpotifySaveFavorites);
    server.on("/api/spotify/volume", HTTP_POST, handleSpotifyVolume);
    server.on("/api/spotify/exit", HTTP_POST, handleSpotifyExit);
    server.on("/api/spotify/play", HTTP_POST, []() {
        lastWebActivityTime = millis();
        SpotifyCommand cmd = {SPOTIFY_CMD_PLAY, 0, false, ""};
        postSpotifyCommand(cmd);
        currentState = STATE_MUSIC_MODE;
        server.send(200, "text/plain", "OK");
    });
    server.on("/api/spotify/pause", HTTP_POST, []() {
        lastWebActivityTime = millis();
        SpotifyCommand cmd = {SPOTIFY_CMD_PAUSE, 0, false, ""};
        postSpotifyCommand(cmd);
        server.send(200, "text/plain", "OK");
    });
    server.on("/api/spotify/next", HTTP_POST, []() {
        lastWebActivityTime = millis();
        SpotifyCommand cmd = {SPOTIFY_CMD_NEXT, 0, false, ""};
        postSpotifyCommand(cmd);
        server.send(200, "text/plain", "OK");
    });
    server.on("/api/spotify/prev", HTTP_POST, []() {
        lastWebActivityTime = millis();
        SpotifyCommand cmd = {SPOTIFY_CMD_PREV, 0, false, ""};
        postSpotifyCommand(cmd);
        server.send(200, "text/plain", "OK");
    });
    server.on("/api/spotify/track", HTTP_GET, handleGetSpotifyTrack);
    server.on("/api/spotify/debug", HTTP_GET, []() {
        lastWebActivityTime = millis();
        JsonDocument doc;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["minFreeHeap"] = ESP.getMinFreeHeap();
        doc["currentState"] = (int)currentState;
        doc["isPlaying"] = getSpotifyIsPlaying();
        doc["volumePercent"] = getSpotifyVolumePercent();
        doc["track"] = getSpotifyTrackText();
        doc["lastError"] = getSpotifyLastError();
        doc["sleepRemainingMinutes"] = (sleepTimerEnd > millis()) ? ((sleepTimerEnd - millis() + 59999) / 60000) : 0;
        doc["totalHttpRequests"] = s_totalRequests;
        doc["lastClientIp"] = s_lastClientIp;
        doc["lastRequestUri"] = s_lastRequestUri;
        doc["lastRequestSecondsAgo"] = (s_lastRequestTime > 0) ? (long)((millis() - s_lastRequestTime) / 1000) : -1;
        String resp;
        serializeJson(doc, resp);
        sendNoCacheHeaders();
        server.send(200, "application/json", resp);
    });

    // Audio engine remote endpoints
    server.on("/api/audio/noise_on", HTTP_POST, handleAudioNoiseOn);
    server.on("/api/audio/noise_off", HTTP_POST, handleAudioNoiseOff);
    
    // School Calendar API endpoint
    server.on("/api/school_calendar", HTTP_GET, handleGetSchoolCalendar);
    
    // Countdown Timer endpoints
    server.on("/api/timer/start", HTTP_POST, []() {
        lastWebActivityTime = millis();
        if (rejectOversizedBody(512)) return;
        if (!server.hasArg("plain")) {
            server.send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        int minutes = doc["minutes"].as<int>();
        if (minutes <= 0) minutes = 15;
        startCountdownTimer(minutes);
        clockScreenIndex = 1; // Peek at Card 1
        screenCarouselTimeout = millis() + 10000;
        lastLCDUpdate = 0;
        server.send(200, "application/json", "{\"success\":true}");
    });

    server.on("/api/timer/sleep", HTTP_POST, []() {
        lastWebActivityTime = millis();
        if (!server.hasArg("plain")) {
            server.send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        int minutes = doc["minutes"].as<int>();
        sleepDurationMinutes = minutes;
        if (minutes > 0) {
            sleepTimerEnd = millis() + (minutes * 60 * 1000L);
            if (hueLightsEnabled) {
                startHueSleepTimerFade(minutes);
            }
            Serial.printf("[Web] Sleep timer set for %d minutes.\r\n", minutes);
        } else {
            sleepTimerEnd = 0;
            Serial.println("[Web] Sleep timer cancelled.");
        }
        lastLCDUpdate = 0;
        server.send(200, "application/json", "{\"success\":true}");
    });

    server.on("/api/timer/stop", HTTP_POST, []() {
        lastWebActivityTime = millis();
        stopCountdownTimer();
        clockScreenIndex = 0;
        lastLCDUpdate = 0;
        server.send(200, "application/json", "{\"success\":true}");
    });

    server.on("/api/timer/test_sound", HTTP_POST, []() {
        lastWebActivityTime = millis();
        playNamedSound("siren");
        server.send(200, "application/json", "{\"success\":true}");
    });

    // Start HTTP Server
    server.begin();
    Serial.println("[HTTP] Web Server started on port 80.");
}

void cancelVoiceMemoPlayback() {
    stopAudioPlayback();
}

void updateWebServer() {
    server.handleClient();
}
