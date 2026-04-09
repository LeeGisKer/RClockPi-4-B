#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "db/EventStore.h"
#include "services/CalendarSyncService.h"
#include "services/WeatherSyncService.h"
#include "util/TimeUtil.h"
#include "views/CalendarView.h"
#include "views/ClockView.h"
#include "views/WeatherView.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {

constexpr size_t kMaxConfigBytes = 64 * 1024;
constexpr size_t kMaxPathBytes = 512;
constexpr size_t kMaxUrlBytes = 2048;

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

bool ContainsControlChars(const std::string& value) {
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) {
            return true;
        }
    }
    return false;
}

bool ReadFileWithLimit(const std::string& path, size_t max_bytes, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open config: " << path << "\n";
        return false;
    }

    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) {
        std::cerr << "Failed to determine config size.\n";
        return false;
    }
    if (static_cast<size_t>(size) > max_bytes) {
        std::cerr << "Config file is too large.\n";
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

bool ReadIntInRange(const nlohmann::json& j, const char* key, int min_value, int max_value, int* out) {
    if (!j.contains(key)) {
        return true;
    }
    const auto& value = j.at(key);
    if (!value.is_number_integer()) {
        std::cerr << "Config key '" << key << "' must be an integer.\n";
        return false;
    }
    int parsed = value.get<int>();
    if (parsed < min_value || parsed > max_value) {
        std::cerr << "Config key '" << key << "' is out of range.\n";
        return false;
    }
    *out = parsed;
    return true;
}

bool ReadBool(const nlohmann::json& j, const char* key, bool* out) {
    if (!j.contains(key)) {
        return true;
    }
    const auto& value = j.at(key);
    if (!value.is_boolean()) {
        std::cerr << "Config key '" << key << "' must be a boolean.\n";
        return false;
    }
    *out = value.get<bool>();
    return true;
}

bool ReadDoubleInRange(const nlohmann::json& j, const char* key, double min_value, double max_value, double* out) {
    if (!j.contains(key)) {
        return true;
    }
    const auto& value = j.at(key);
    if (!value.is_number()) {
        std::cerr << "Config key '" << key << "' must be numeric.\n";
        return false;
    }
    double parsed = value.get<double>();
    if (!std::isfinite(parsed) || parsed < min_value || parsed > max_value) {
        std::cerr << "Config key '" << key << "' is out of range.\n";
        return false;
    }
    *out = parsed;
    return true;
}

bool ReadPathString(const nlohmann::json& j, const char* key, size_t max_bytes, std::string* out) {
    if (!j.contains(key)) {
        return true;
    }
    const auto& value = j.at(key);
    if (!value.is_string()) {
        std::cerr << "Config key '" << key << "' must be a string.\n";
        return false;
    }
    std::string parsed = Trim(value.get<std::string>());
    if (parsed.empty() || parsed.size() > max_bytes || ContainsControlChars(parsed)) {
        std::cerr << "Config key '" << key << "' is malformed or too large.\n";
        return false;
    }
    *out = parsed;
    return true;
}

std::filesystem::path ResolvePath(const std::filesystem::path& config_path,
                                  const std::string& value,
                                  bool allow_parent_fallback) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path input(value);
    if (input.is_absolute()) {
        return input;
    }
    std::filesystem::path base = std::filesystem::absolute(config_path).parent_path();
    std::filesystem::path candidate = base / input;
    if (!allow_parent_fallback) {
        return candidate;
    }
    std::filesystem::path fallback = base.parent_path() / input;
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }
    if (std::filesystem::exists(fallback)) {
        return fallback;
    }
    if (!std::filesystem::exists(candidate.parent_path()) && std::filesystem::exists(fallback.parent_path())) {
        return fallback;
    }
    return candidate;
}

class CurlGlobalGuard {
public:
    CurlGlobalGuard() : initialized_(curl_global_init(CURL_GLOBAL_DEFAULT) == 0) {}
    ~CurlGlobalGuard() {
        if (initialized_) {
            curl_global_cleanup();
        }
    }
    bool IsInitialized() const { return initialized_; }
private:
    bool initialized_ = false;
};

} // namespace

struct AppConfig {
    int sync_interval_sec = 120;
    int time_window_days = 14;
    int idle_threshold_sec = 30;
    bool night_mode_enabled = true;
    int night_start_hour = 21;
    int night_end_hour = 6;
    int night_dim_alpha = 110;
    std::string font_path = "./assets/DejaVuSans.ttf";
    std::string db_path = "./data/calendar.db";
    bool mock_mode = true;
    std::string ics_url;
    bool weather_enabled = false;
    double weather_latitude = 0.0;
    double weather_longitude = 0.0;
    int weather_sync_interval_sec = 900;
    std::string weather_sprite_dir = "./assets/weather";
    std::string sprite_dir = "./assets/sprites";
};

bool LoadConfig(const std::string& path, AppConfig* out) {
    std::string raw;
    if (!ReadFileWithLimit(path, kMaxConfigBytes, &raw)) {
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse config: " << ex.what() << "\n";
        return false;
    }
    if (!j.is_object()) {
        std::cerr << "Config root must be a JSON object.\n";
        return false;
    }

    if (!ReadIntInRange(j, "sync_interval_sec", 5, 24 * 60 * 60, &out->sync_interval_sec) ||
        !ReadIntInRange(j, "time_window_days", 1, 365, &out->time_window_days) ||
        !ReadIntInRange(j, "idle_threshold_sec", 5, 24 * 60 * 60, &out->idle_threshold_sec) ||
        !ReadIntInRange(j, "night_start_hour", 0, 23, &out->night_start_hour) ||
        !ReadIntInRange(j, "night_end_hour", 0, 23, &out->night_end_hour) ||
        !ReadIntInRange(j, "night_dim_alpha", 0, 255, &out->night_dim_alpha) ||
        !ReadIntInRange(j, "weather_sync_interval_sec", 60, 24 * 60 * 60, &out->weather_sync_interval_sec) ||
        !ReadBool(j, "night_mode_enabled", &out->night_mode_enabled) ||
        !ReadBool(j, "weather_enabled", &out->weather_enabled) ||
        !ReadBool(j, "mock_mode", &out->mock_mode) ||
        !ReadDoubleInRange(j, "weather_latitude", -90.0, 90.0, &out->weather_latitude) ||
        !ReadDoubleInRange(j, "weather_longitude", -180.0, 180.0, &out->weather_longitude) ||
        !ReadPathString(j, "font_path", kMaxPathBytes, &out->font_path) ||
        !ReadPathString(j, "db_path", kMaxPathBytes, &out->db_path) ||
        !ReadPathString(j, "weather_sprite_dir", kMaxPathBytes, &out->weather_sprite_dir) ||
        !ReadPathString(j, "sprite_dir", kMaxPathBytes, &out->sprite_dir)) {
        return false;
    }

    return true;
}

int main(int argc, char** argv) {
    std::string config_path = "config/config.json";
    if (argc > 1) {
        config_path = argv[1];
        if (config_path.empty() || config_path.size() > kMaxPathBytes || ContainsControlChars(config_path)) {
            std::cerr << "Config path is malformed or too large.\n";
            return 1;
        }
    }

    AppConfig config;
    if (!LoadConfig(config_path, &config)) {
        return 1;
    }

    const char* env_ics = std::getenv("ICS_URL");
    if (env_ics && env_ics[0] != '\0') {
        config.ics_url = Trim(env_ics);
    } else {
        config.ics_url.clear();
    }
    if (!config.ics_url.empty() &&
        (config.ics_url.size() > kMaxUrlBytes || ContainsControlChars(config.ics_url))) {
        std::cerr << "ICS_URL is malformed or too large.\n";
        return 1;
    }
    if (!config.mock_mode && config.ics_url.empty()) {
        std::cerr << "No ICS URL configured. Running in cache-only mode.\n";
    }

    std::filesystem::path config_abs = std::filesystem::absolute(config_path);
    config.font_path = ResolvePath(config_abs, config.font_path, true).string();
    config.sprite_dir = ResolvePath(config_abs, config.sprite_dir, true).string();
    config.weather_sprite_dir = ResolvePath(config_abs, config.weather_sprite_dir, true).string();
    config.db_path = ResolvePath(config_abs, config.db_path, true).string();

    std::filesystem::create_directories(std::filesystem::path(config.db_path).parent_path());

    EventStore store(config.db_path);
    if (!store.Open()) {
        std::cerr << "Failed to open database." << "\n";
        return 1;
    }

    SyncConfig sync_config;
    sync_config.db_path = config.db_path;
    sync_config.sync_interval_sec = config.sync_interval_sec;
    sync_config.time_window_days = config.time_window_days;
    sync_config.mock_mode = config.mock_mode;
    sync_config.ics_url = config.ics_url;

    CurlGlobalGuard curl_guard;
    if (!curl_guard.IsInitialized()) {
        std::cerr << "libcurl global init failed\n";
        return 1;
    }

    CalendarSyncService sync_service(sync_config);
    sync_service.Start();

    WeatherConfig weather_config;
    weather_config.db_path = config.db_path;
    weather_config.enabled = config.weather_enabled;
    weather_config.latitude = config.weather_latitude;
    weather_config.longitude = config.weather_longitude;
    weather_config.sync_interval_sec = std::max(60, config.weather_sync_interval_sec);

    WeatherSyncService weather_service(weather_config);
    weather_service.Start();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
        std::cerr << "IMG_Init failed: " << IMG_GetError() << "\n";
    }

    SDL_Window* window = SDL_CreateWindow(
        "RPI Calendar",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800,
        480,
        SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font* font_time = TTF_OpenFont(config.font_path.c_str(), 80);
    TTF_Font* font_date = TTF_OpenFont(config.font_path.c_str(), 18);
    TTF_Font* font_info = TTF_OpenFont(config.font_path.c_str(), 16);
    TTF_Font* font_header = TTF_OpenFont(config.font_path.c_str(), 18);
    TTF_Font* font_day = TTF_OpenFont(config.font_path.c_str(), 16);
    TTF_Font* font_agenda = TTF_OpenFont(config.font_path.c_str(), 16);
    TTF_Font* font_weather_temp = TTF_OpenFont(config.font_path.c_str(), 50);

    if (!font_time || !font_date || !font_info || !font_header || !font_day || !font_agenda || !font_weather_temp) {
        std::cerr << "Failed to load font: " << config.font_path << "\n";
        if (font_time) TTF_CloseFont(font_time);
        if (font_date) TTF_CloseFont(font_date);
        if (font_info) TTF_CloseFont(font_info);
        if (font_header) TTF_CloseFont(font_header);
        if (font_day) TTF_CloseFont(font_day);
        if (font_agenda) TTF_CloseFont(font_agenda);
        if (font_weather_temp) TTF_CloseFont(font_weather_temp);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    {
        ClockView clock_view(renderer, font_time, font_date, font_info, &store, config.sprite_dir);
        CalendarView calendar_view(renderer, font_header, font_day, font_agenda, &store);
        WeatherView weather_view(renderer, font_header, font_info, font_weather_temp, &store, config.weather_sprite_dir);

        enum class ViewMode { Clock, Calendar, Weather };
        ViewMode current_view = ViewMode::Clock;

        auto last_input = std::chrono::steady_clock::now();
        bool capture_next_frame = false;

        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    running = false;
                } else if (ev.type == SDL_KEYDOWN) {
                    last_input = std::chrono::steady_clock::now();
                    switch (ev.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_SPACE:
                            if (current_view == ViewMode::Clock) {
                                current_view = ViewMode::Calendar;
                            } else if (current_view == ViewMode::Calendar) {
                                current_view = ViewMode::Weather;
                            } else {
                                current_view = ViewMode::Clock;
                            }
                            break;
                        case SDLK_s:
                            capture_next_frame = true;
                            break;
                        case SDLK_LEFT:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveSelectionDays(-1);
                            }
                            break;
                        case SDLK_RIGHT:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveSelectionDays(1);
                            }
                            break;
                        case SDLK_UP:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveSelectionDays(-7);
                            }
                            break;
                        case SDLK_DOWN:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveSelectionDays(7);
                            }
                            break;
                        case SDLK_n:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveMonth(1);
                            }
                            break;
                        case SDLK_m:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.MoveMonth(-1);
                            }
                            break;
                        case SDLK_t:
                            if (current_view == ViewMode::Calendar) {
                                calendar_view.JumpToToday();
                            }
                            break;
                        default:
                            break;
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - last_input).count();
            if (idle_sec >= config.idle_threshold_sec) {
                current_view = ViewMode::Clock;
            }

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderClear(renderer);

            int w = 0, h = 0;
            SDL_GetRendererOutputSize(renderer, &w, &h);

            if (current_view == ViewMode::Clock) {
                clock_view.Render(w, h);
            } else if (current_view == ViewMode::Calendar) {
                calendar_view.Render(w, h);
            } else {
                weather_view.Render(w, h);
            }

            if (config.night_mode_enabled) {
                std::tm now_tm = TimeUtil::LocalTime(TimeUtil::NowTs());
                int hour = now_tm.tm_hour;
                bool is_night = false;
                if (config.night_start_hour == config.night_end_hour) {
                    is_night = false;
                } else if (config.night_start_hour < config.night_end_hour) {
                    is_night = (hour >= config.night_start_hour && hour < config.night_end_hour);
                } else {
                    is_night = (hour >= config.night_start_hour || hour < config.night_end_hour);
                }

                if (is_night && config.night_dim_alpha > 0) {
                    int alpha = std::min(255, std::max(0, config.night_dim_alpha));
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(alpha));
                    SDL_Rect dim_rect{ 0, 0, w, h };
                    SDL_RenderFillRect(renderer, &dim_rect);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                }
            }

            SDL_RenderPresent(renderer);

            if (capture_next_frame) {
                SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                if (shot) {
                    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0) {
                        SDL_SaveBMP(shot, "data/preview.bmp");
                    } else {
                        std::cerr << "SDL_RenderReadPixels failed: " << SDL_GetError() << "\n";
                    }
                    SDL_FreeSurface(shot);
                } else {
                    std::cerr << "SDL_CreateRGBSurfaceWithFormat failed: " << SDL_GetError() << "\n";
                }
                capture_next_frame = false;
            }

            SDL_Delay(33);
        }
    }

    weather_service.Stop();
    sync_service.Stop();

    TTF_CloseFont(font_time);
    TTF_CloseFont(font_date);
    TTF_CloseFont(font_info);
    TTF_CloseFont(font_header);
    TTF_CloseFont(font_day);
    TTF_CloseFont(font_agenda);
    TTF_CloseFont(font_weather_temp);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();

    return 0;
}
