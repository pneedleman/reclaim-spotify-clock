#include "timer_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t timerMutex = NULL;
static bool s_active = false;
static unsigned long s_startTime = 0;
static unsigned long s_durationMs = 0;
static bool s_justFinished = false;

void setupTimerManager() {
    if (!timerMutex) {
        timerMutex = xSemaphoreCreateMutex();
    }
}

void startCountdownTimer(int minutes) {
    if (minutes <= 0) return;
    setupTimerManager();
    
    if (timerMutex && xSemaphoreTake(timerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_active = true;
        s_durationMs = (unsigned long)minutes * 60UL * 1000UL;
        s_startTime = millis();
        s_justFinished = false;
        xSemaphoreGive(timerMutex);
        Serial.printf("[Timer] Started %d minute countdown timer.\r\n", minutes);
    }
}

void stopCountdownTimer() {
    setupTimerManager();
    if (timerMutex && xSemaphoreTake(timerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_active = false;
        s_durationMs = 0;
        s_startTime = 0;
        s_justFinished = false;
        xSemaphoreGive(timerMutex);
        Serial.println("[Timer] Timer stopped / cleared.");
    }
}

bool isCountdownTimerActive() {
    bool act = false;
    setupTimerManager();
    if (timerMutex && xSemaphoreTake(timerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active) {
            unsigned long elapsed = millis() - s_startTime;
            if (elapsed >= s_durationMs) {
                s_active = false;
                s_justFinished = true;
                act = false;
            } else {
                act = true;
            }
        }
        xSemaphoreGive(timerMutex);
    }
    return act;
}

TimerSnapshot getTimerSnapshot() {
    TimerSnapshot snap;
    setupTimerManager();
    if (timerMutex && xSemaphoreTake(timerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active) {
            unsigned long now = millis();
            unsigned long elapsed = now - s_startTime;
            if (elapsed >= s_durationMs) {
                s_active = false;
                s_justFinished = true;
                snap.active = false;
                snap.remainingSeconds = 0;
                snap.totalSeconds = s_durationMs / 1000UL;
                snap.progress = 1.0f;
                snap.timeString = "00:00";
            } else {
                snap.active = true;
                snap.totalSeconds = s_durationMs / 1000UL;
                unsigned long remainingMs = s_durationMs - elapsed;
                snap.remainingSeconds = (remainingMs + 999UL) / 1000UL; // round up to whole seconds
                snap.progress = (float)elapsed / (float)s_durationMs;
                
                int mins = snap.remainingSeconds / 60;
                int secs = snap.remainingSeconds % 60;
                char buf[10];
                snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
                snap.timeString = String(buf);
            }
        } else {
            snap.active = false;
            snap.remainingSeconds = 0;
            snap.totalSeconds = 0;
            snap.progress = 0.0f;
            snap.timeString = "00:00";
        }
        xSemaphoreGive(timerMutex);
    }
    return snap;
}

bool checkTimerJustFinished() {
    bool finished = false;
    setupTimerManager();
    if (timerMutex && xSemaphoreTake(timerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active) {
            unsigned long elapsed = millis() - s_startTime;
            if (elapsed >= s_durationMs) {
                s_active = false;
                s_justFinished = true;
            }
        }
        if (s_justFinished) {
            finished = true;
            s_justFinished = false; // reset flag
        }
        xSemaphoreGive(timerMutex);
    }
    return finished;
}
