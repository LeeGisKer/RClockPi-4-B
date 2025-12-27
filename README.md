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

3) Build:

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j4
```

4) Run (fullscreen):

```bash
./rpi_calendar ../config/config.json
```

Keys: `Space` toggles views, `Esc` quits, `S` saves a screenshot to `data/preview.bmp`.

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
./rpi_calendar ../config/config.json
```

The app runs fullscreen by default. Press `Esc` to quit.

## Config

Copy `config/config.example.json` to `config/config.json` and edit it:

- `font_path`: path to a TTF font file (required)
- `db_path`: SQLite cache path
- `mock_mode`: `true` to seed sample events for UI testing
- `idle_threshold_sec`, `auto_cycle_clock_sec`, `auto_cycle_calendar_sec`: idle auto-cycle timing
- `sync_interval_sec`, `time_window_days`: sync behavior
- `sprite_dir`: folder for time-of-day sprites (default `./assets/sprites`)
- `night_mode_enabled`, `night_start_hour`, `night_end_hour`, `night_dim_alpha`: dim the screen during night hours
- `ICS_URL` environment variable is required and kept out of the repo.

## Using a secret iCal (ICS) URL

1) Copy your calendar's **secret iCal URL** from Google Calendar settings.
2) Set `ICS_URL` as an environment variable (required).
3) Set `mock_mode` to `false`.

Example:

```bash
ICS_URL="https://calendar.google.com/calendar/ical/.../basic.ics" ./rpi_calendar ../config/config.json
```

## Mock mode

Set `"mock_mode": true` to insert sample events into the SQLite cache on startup. This is enough to test the UI without Google Calendar.
