#!/usr/bin/env python3
"""
School Calendar ICS to JSON Converter
Reclaim Spotify Bedside Clock

Usage:
    python3 ics_to_json.py https://example-school.org/calendar.ics
    python3 ics_to_json.py local_calendar.ics

Outputs:
    school_calendar.json (upload to clock SPIFFS or host on GitHub raw)
"""

import sys
import json
import urllib.request
import re
from datetime import datetime

# Keywords used to identify school holidays / days off
HOLIDAY_KEYWORDS = [
    "no school", "holiday", "break", "in-service", "professional day",
    "teacher day", "recess", "closed", "vacation", "snow day", "labor day",
    "thanksgiving", "christmas", "new year", "mlk", "presidents", "memorial day"
]

def fetch_ics_content(source):
    if source.startswith("http://") or source.startswith("https://"):
        print(f"Fetching remote ICS calendar from: {source}...")
        req = urllib.request.Request(source, headers={'User-Agent': 'Mozilla/5.0'})
        with urllib.request.urlopen(req) as resp:
            return resp.read().decode('utf-8', errors='ignore')
    else:
        print(f"Reading local ICS file: {source}...")
        with open(source, 'r', encoding='utf-8', errors='ignore') as f:
            return f.read()

def parse_ics(ics_text):
    events = []
    current_event = {}
    in_event = False

    for line in ics_text.splitlines():
        line = line.strip()
        if line.startswith("BEGIN:VEVENT"):
            in_event = True
            current_event = {}
        elif line.startswith("END:VEVENT"):
            if in_event and "date" in current_event and "name" in current_event:
                events.append(current_event)
            in_event = False
        elif in_event:
            if line.startswith("SUMMARY:"):
                current_event["name"] = line[8:].strip()
            elif line.startswith("DTSTART;VALUE=DATE:") or line.startswith("DTSTART:"):
                dt_str = line.split(":")[-1].strip()
                match = re.match(r"(\d{4})(\d{2})(\d{2})", dt_str)
                if match:
                    current_event["date"] = f"{match.group(1)}-{match.group(2)}-{match.group(3)}"

    # Filter for holiday/no-school events and deduplicate by date
    filtered_events = []
    seen_dates = set()

    for ev in events:
        name_lower = ev["name"].lower()
        if any(kw in name_lower for kw in HOLIDAY_KEYWORDS):
            if ev["date"] not in seen_dates:
                seen_dates.add(ev["date"])
                filtered_events.append({
                    "date": ev["date"],
                    "name": ev["name"]
                })

    filtered_events.sort(key=lambda x: x["date"])
    return filtered_events

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 ics_to_json.py <ICS_URL_OR_FILE_PATH>")
        sys.exit(1)

    source = sys.argv[1]
    ics_text = fetch_ics_content(source)
    events = parse_ics(ics_text)

    output_file = "school_calendar.json"
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(events, f, indent=2)

    print(f"✅ Successfully converted {len(events)} school day-off events!")
    print(f"Saved to: {output_file}")

if __name__ == "__main__":
    main()
