#ifndef GCAL_CLIENT_H
#define GCAL_CLIENT_H

#include "globals.h"

struct GCalEvent {
    String title;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = -1;       // 0-23, or -1 if all-day
    int minute = 0;
    bool isAllDay = false;
    bool isToday = false;
    bool isTomorrow = false;
    String displayString; // e.g. "Next: Swim 9:00a" or "Tmw: Soccer 10a"
};

struct GCalSnapshot {
    bool enabled = true;
    bool loaded = false;
    int eventCount = 0;
    GCalEvent nextEvent;
    String filterKeyword = "Faye";
    int lookaheadHours = 48;
};

void setupGCal();
void updateGCal();
void triggerImmediateGCalSync();
GCalSnapshot getGCalSnapshot();
void setGCalUrl(const String& url);
String getGCalUrl();
void setGCalFilterKeyword(const String& kw);
String getGCalFilterKeyword();
void setGCalLookaheadHours(int hours);
int getGCalLookaheadHours();
String getGCalNoEventsMessage(int lookaheadHours = 48);
void setGCalEnabled(bool enabled);
bool isGCalEnabled();

#endif // GCAL_CLIENT_H
