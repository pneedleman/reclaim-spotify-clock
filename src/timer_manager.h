#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <Arduino.h>

struct TimerSnapshot {
    bool active = false;
    int remainingSeconds = 0;
    int totalSeconds = 0;
    float progress = 0.0f; // 0.0 to 1.0 (elapsed fraction)
    String timeString;    // "14:28"
};

void setupTimerManager();
void startCountdownTimer(int minutes);
void stopCountdownTimer();
bool isCountdownTimerActive();
TimerSnapshot getTimerSnapshot();
bool checkTimerJustFinished(); // Returns true once when timer hits 00:00

#endif // TIMER_MANAGER_H
