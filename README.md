# Raspberry Pi Calendar Kiosk

Fullscreen smart clock + calendar for Raspberry Pi OS using SDL2 + SDL2_ttf.

## Dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libsdl2-dev libsdl2-ttf-dev \
  libcurl4-openssl-dev libsqlite3-dev \
  nlohmann-json3-dev
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

Edit `config/config.json`:

- `font_path`: path to a TTF font file (required)
- `db_path`: SQLite cache path
- `mock_mode`: `true` to seed sample events for UI testing
- `idle_threshold_sec`, `auto_cycle_clock_sec`, `auto_cycle_calendar_sec`: idle auto-cycle timing
- `calendar_ids`: list of Google Calendar IDs (default `primary`)
- `sync_interval_sec`, `time_window_days`: sync behavior
- `token_path`: OAuth token file

## OAuth setup (placeholder)

OAuth refresh token flow is scaffolded but not yet implemented. For now:

1) Create `config/token.json` manually with your refresh token once available.
2) Set `mock_mode` to `false` to enable real sync once implemented.

Example `token.json` format:

```json
{
  "access_token": "",
  "refresh_token": "YOUR_REFRESH_TOKEN",
  "expiry_ts": 0,
  "token_type": "Bearer"
}
```

## Mock mode

Set `"mock_mode": true` to insert sample events into the SQLite cache on startup. This is enough to test the UI without Google Calendar.
