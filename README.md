# Raspberry Pi Calendar Kiosk

Fullscreen smart clock + calendar for Raspberry Pi OS using SDL2 + SDL2_ttf.

## Dependencies (Raspberry Pi OS)

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
  libcurl4-openssl-dev libsqlite3-dev \
  nlohmann-json3-dev \
  ca-certificates \
  fonts-dejavu-core
```

## Raspberry Pi run guide

1) Clone or copy the project onto the Pi.

2) Put a TTF font on the Pi and update `config/config.json`:
   - Recommended: copy a font into `assets/` and set `"font_path": "./assets/DejaVuSans.ttf"`.

3) Set the secret calendar URL as an environment variable:

```bash
export ICS_URL="https://calendar.google.com/calendar/ical/.../basic.ics"
```

PowerShell:

```powershell
$env:ICS_URL = "https://calendar.google.com/calendar/ical/.../basic.ics"
```

4) Build:

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j4
```

5) Run (fullscreen):

```bash
./run_clock.sh
```

Keys: `Space` cycles views (Clock -> Calendar -> Weather), `Esc` quits, `S` saves a screenshot to `data/preview.bmp`.

### Common Pi notes

- If SDL fails to open a display, run from the desktop session or try:

```bash
export SDL_VIDEODRIVER=kmsdrm
```

- For kiosk-style boot, add a systemd service that runs the binary on startup.

## Build

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j4
```

## Run

```bash
./run_clock.sh
```

The app runs fullscreen by default. Press `Esc` to quit.

`run_clock.sh` behavior:
- Disables X11 screen blanking/DPMS when available.
- Uses `systemd-inhibit` (when installed) to block idle sleep/shutdown while the app is running.
- Restarts the app automatically if it exits unexpectedly.
- Writes logs to `logs/rpi_calendar.log`.

Optional launcher env vars:
- `RESTART_ON_EXIT=0` disable auto-restart loop
- `RESTART_DELAY_SEC=2` seconds before restart
- `LOG_FILE=/path/to/file.log` custom log path

## Config

Copy `config/config.example.json` to `config/config.json` and edit it:

- `font_path`: path to a TTF font file (required)
- `db_path`: SQLite cache path
- `mock_mode`: `true` to seed sample events for UI testing
- `idle_threshold_sec`: seconds before returning to Clock view when idle
- `sync_interval_sec`, `time_window_days`: sync behavior
- `weather_enabled`: enable live weather sync
- `weather_latitude`, `weather_longitude`: coordinates for weather lookup
- `weather_sync_interval_sec`: weather refresh interval (seconds)
- `weather_sprite_dir`: folder with weather condition sprites
- `sprite_dir`: folder for time-of-day sprites (default `./assets/sprites`)
- `night_mode_enabled`, `night_start_hour`, `night_end_hour`, `night_dim_alpha`: dim the screen during night hours
- `sprite_dir`: folder for time-of-day sprites (default `./assets/sprites`)
- `night_mode_enabled`, `night_start_hour`, `night_end_hour`, `night_dim_alpha`: dim the screen during night hours
- `ICS_URL` is not stored in `config.json`; set it in the environment instead.

## Offline behavior

- The app keeps events in SQLite and continues running if internet is down.
- If `ICS_URL` is not configured and `mock_mode` is `false`, calendar sync is unavailable.
- Increase `time_window_days` if you need to prefetch more days before going offline.
- Weather uses Open-Meteo (no API key); if internet drops it shows the last cached weather data.

## Weather setup

1) In `config/config.json`, set:
   - `"weather_enabled": true`
   - `"weather_latitude": <your-latitude>`
   - `"weather_longitude": <your-longitude>`
2) Keep `"weather_sync_interval_sec"` around `600-1800` for kiosk use.
3) Add your custom sprites to `weather_sprite_dir` using these filenames:
   - `clear.png`, `clear_night.png`, `mostly_clear.png`, `partly_cloudy.png`
   - `overcast.png`, `fog.png`, `drizzle.png`, `rain.png`
   - `snow.png`, `showers.png`, `thunder.png`, `unknown.png`
4) Restart the app and press `Space` until the Weather page is visible.
5) The Weather page shows current conditions, hourly forecast, and 7-day forecast.

## If the Pi really powers off after hours

If the whole Raspberry Pi loses power (not just a blank screen), this is usually hardware/power/thermal related:

1) Check undervoltage / throttling:
```bash
vcgencmd get_throttled
```
If non-zero bits appear often, use a stronger PSU/cable.

2) Check temperature while running:
```bash
vcgencmd measure_temp
```
Sustained high temperature can trigger instability.

3) Check reboot/shutdown reasons:
```bash
journalctl -b -1 -e
```
Look for `Under-voltage`, `thermal`, `shutdown`, `kernel panic`.

## Using a secret iCal (ICS) URL

1) Copy your calendar's **secret iCal URL** from Google Calendar settings.
2) Export it as `ICS_URL` in the shell, service, or launcher that starts the app.
3) Use `.env.example` only as a local template if your own launcher loads `.env`; do not commit `.env`.
4) Set `"mock_mode": false` in `config/config.json`.

## Mock mode

Set `"mock_mode": true` to insert sample events into the SQLite cache on startup. This is enough to test the UI without Google Calendar.
