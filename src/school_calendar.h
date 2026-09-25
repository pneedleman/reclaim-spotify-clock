#ifndef SCHOOL_CALENDAR_H
#define SCHOOL_CALENDAR_H

#include <Arduino.h>
#include <time.h>

// Maximum number of school days off to store (current + next school year)
#define MAX_SCHOOL_DAYS_OFF 50

// School day off structure
struct SchoolDayOff {
    int year;
    int month;  // 1-12
    int day;    // 1-31
    String description;  // e.g., "Winter Break", "Thanksgiving"
};

// Global variables
extern SchoolDayOff schoolDaysOff[MAX_SCHOOL_DAYS_OFF];
extern int numSchoolDaysOff;
extern bool schoolCalendarLoaded;
extern unsigned long lastSchoolCalendarUpdate;

// Function declarations
void setupSchoolCalendar();
void updateSchoolCalendar();
bool isSchoolDayOff(int year, int month, int day);
bool isSchoolDay(struct tm* timeinfo);
String getNextSchoolDayOff(struct tm* currentDate, int* daysUntil = nullptr);
String getTodaySchoolDayOffName(struct tm* currentDate);
int daysUntilDate(int year, int month, int day, struct tm* currentDate);
void getSchoolDayOffDescription(int index, String& desc, int& month, int& day);

#endif // SCHOOL_CALENDAR_H
