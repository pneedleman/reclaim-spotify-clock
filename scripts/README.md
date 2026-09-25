# 🛠️ Reclaim Bedside Clock Helper Scripts

Helper tools for authorizing Spotify and converting school calendar feeds.

---

## 🔑 1. Spotify Refresh Token Generator (`get_spotify_refresh_token.py`)

Generate a long-lived `SECRET_SPOTIFY_REFRESH_TOKEN` for your clock in 30 seconds:

1. Create a free Spotify Developer App at [developer.spotify.com](https://developer.spotify.com/dashboard).
2. In your Spotify app settings, add `http://127.0.0.1:8888/callback` as a **Redirect URI**.
3. Run the script with your Client ID and Client Secret:

```bash
python3 scripts/get_spotify_refresh_token.py <SPOTIFY_CLIENT_ID> <SPOTIFY_CLIENT_SECRET>
```

The script opens your browser to authorize your app, receives the code on `127.0.0.1:8888`, and prints your `SECRET_SPOTIFY_REFRESH_TOKEN` to paste into `src/secrets.h`!

---

## 📅 2. School Calendar ICS Converter (`ics_to_json.py`)

Convert any school district's `.ics` (iCalendar) feed into a lightweight `school_calendar.json` file for your clock:

```bash
python3 scripts/ics_to_json.py https://example-school.org/calendar.ics
```

### How to Use the Generated File:
- **Offline Mode**: Copy `school_calendar.json` into `data/` and run `pio run -t uploadfs`.
- **Online Mode**: Push `school_calendar.json` to GitHub and set `SCHOOL_CALENDAR_JSON_URL` in `src/config.h`.
