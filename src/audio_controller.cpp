#include "audio_controller.h"
#include "spotify_client.h" // We need sendSpotifyCommand

// Define audio object
Audio audio;

// Define shared variables
int currentVolume = 10;
bool isMuted = false;
unsigned long previewStopTimeout = 0;

static TaskHandle_t fallbackBuzzerTaskHandle = NULL;
static bool fallbackBuzzerActive = false;

#ifndef SIMULATION_MODE
static void fallbackBuzzerTask(void* pvParameters) {
    const int sampleRate = 44100;
    const int frequency = 880;
    const int numSamples = sampleRate / frequency; // 50
    const int halfSamples = numSamples / 2; // 25
    
    int16_t periodBuffer[50 * 2]; // 50 stereo samples
    for (int i = 0; i < numSamples; i++) {
        int16_t val = (i < halfSamples) ? 6000 : -6000;
        periodBuffer[i * 2] = val;     // Left
        periodBuffer[i * 2 + 1] = val; // Right
    }
    
    while (true) {
        if (fallbackBuzzerActive) {
            size_t bytesWritten = 0;
            i2s_write(I2S_NUM_0, periodBuffer, sizeof(periodBuffer), &bytesWritten, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif

static void playFallbackBuzzer() {
    Serial.println("[Audio] CRITICAL: Audio file unavailable. Playing hardware emergency buzzer!");
#ifndef SIMULATION_MODE
    fallbackBuzzerActive = true;
#else
    Serial.println("[Simulation] Mock emergency buzzer active!");
#endif
}

void setupAudio() {
    audio.setPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
    audio.setVolume(currentVolume);
    Serial.printf("[Audio] I2S Audio output set to BCLK=%d, LRCK=%d, DOUT=%d\r\n", 
                  I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);

#ifndef SIMULATION_MODE
    // Create the background fallback buzzer task
    xTaskCreate(
        fallbackBuzzerTask,
        "FallbackBuzzer",
        2048,
        NULL,
        1, // Priority
        &fallbackBuzzerTaskHandle
    );
#endif
}

void startAudioPlayback(const char* path, bool loop) {
    Serial.printf("[Audio] Playing local file: %s (loop=%d)\r\n", path, loop);
    isMuted = false;
    updateAudioVolume();
    fallbackBuzzerActive = false; // Stop fallback buzzer before playing new sound

    // Check if the file exists on SPIFFS
    if (!SPIFFS.exists(path)) {
        Serial.printf("[Audio] Warning: File %s does not exist on SPIFFS.\r\n", path);
        // Fallback 1: Try default buzzer.wav
        if (strcmp(path, "/buzzer.wav") != 0 && SPIFFS.exists("/buzzer.wav")) {
            Serial.println("[Audio] Falling back to default /buzzer.wav");
            path = "/buzzer.wav";
        } else {
            // Fallback 2: Direct I2S emergency buzzer
            playFallbackBuzzer();
            return;
        }
    }
    
    bool ok = audio.connecttoFS(SPIFFS, path);
    if (!ok) {
        Serial.printf("[Audio] connecttoFS failed for %s\r\n", path);
        // Fallback 2: Direct I2S emergency buzzer
        playFallbackBuzzer();
        return;
    }
    audio.setFileLoop(loop);
    Serial.printf("[Audio] connecttoFS ok, running=%d, path=%s, loop=%d\r\n", audio.isRunning(), path, loop);
}

void playNamedSound(const String& soundName) {
    if (soundName == "silent" || soundName.isEmpty()) return;
    String path = "/" + soundName + ".wav";
    if (!SPIFFS.exists(path)) {
        path = "/" + soundName + ".mp3";
    }
    startAudioPlayback(path.c_str());
}

void stopAudioPlayback() {
    Serial.println("[Audio] Stopping playback.");
    fallbackBuzzerActive = false; // Stop fallback buzzer if active
    audio.stopSong();
}

void updateAudioVolume() {
    if (isMuted) {
        audio.setVolume(0);
        Serial.println("[Audio] Volume set to 0 (Muted)");
    } else {
        audio.setVolume(currentVolume);
        Serial.printf("[Audio] Volume set to %d\r\n", currentVolume);
    }
}

void toggleMutePlay() {
    if (currentState == STATE_MUSIC_MODE) {
        Serial.println("[Music] Toggling Spotify Play/Pause via command queue.");
        flushPendingSpotifyVolume();
        volumeDisplayTimeout = 0;
        SpotifyCommand cmd = {SPOTIFY_CMD_TOGGLE_PLAY_PAUSE, 0, false, ""};
        postSpotifyCommand(cmd);
        lastLCDUpdate = 0;
    } else {
        isMuted = !isMuted;
        updateAudioVolume();
        
        if (isMuted) {
            Serial.println("CMD:PAUSE");
        } else {
            Serial.println("CMD:PLAY");
        }
        
        volumeDisplayTimeout = millis() + 2000;
        lastLCDUpdate = 0;
    }
}

void previewAlarmSound(int soundIdx) {
    Serial.printf("[Audio] Previewing sound %d: %s\r\n", soundIdx, ALARM_SOUNDS[soundIdx].name);
    startAudioPlayback(ALARM_SOUNDS[soundIdx].path);
    previewStopTimeout = millis() + 2000;
}

void handleAudioPreviewTimeout() {
    if (previewStopTimeout > 0 && millis() >= previewStopTimeout) {
        previewStopTimeout = 0;
        Serial.println("[Audio] Preview stopped automatically.");
        stopAudioPlayback();
    }
}
