# 📅 School Calendar ICS Converter

Convert any school district's `.ics` (iCalendar) feed into a lightweight `school_calendar.json` file for your Reclaim Bedside Clock.

---

## 🚀 Quick Start (1 Command)

Run this Python script with your school district's `.ics` URL (or a local `.ics` file):

```bash
python3 scripts/ics_to_json.py https://example-school.org/calendar.ics
```

This generates `school_calendar.json` containing only the upcoming holidays, teacher workdays, and snow days.

---

## 📲 How to Use the Generated File

You have two choices:

### Option A: Upload Directly to Clock (Offline Mode)
Copy `school_calendar.json` into your `data/` folder and flash SPIFFS:
```bash
cp school_calendar.json data/
pio run -t uploadfs
```
The clock will automatically use this local file to suppress alarms on days off—no internet required!

### Option B: Host on GitHub (Automated Online Mode)
Push `school_calendar.json` to a public GitHub repository and paste the raw URL into `SCHOOL_CALENDAR_JSON_URL` inside `src/config.h`:
```cpp
#define SCHOOL_CALENDAR_JSON_URL "https://raw.githubusercontent.com/your-username/my-clock-data/main/school_calendar.json"
```
The clock will periodically fetch updates over Wi-Fi!
