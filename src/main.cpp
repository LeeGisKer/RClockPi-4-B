#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>

#include <nlohmann/json.hpp>

#include "db/EventStore.h"
#include "services/CalendarSyncService.h"
#include "util/TimeUtil.h"
#include "views/CalendarView.h"
#include "views/ClockView.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

struct AppConfig {
    int sync_interval_sec = 120;
    int time_window_days = 14;
    int idle_threshold_sec = 30;
    int auto_cycle_clock_sec = 1200;
    int auto_cycle_calendar_sec = 1200;
    std::string font_path = "./assets/DejaVuSans.ttf";
    std::string db_path = "./data/calendar.db";
    bool mock_mode = true;
    std::string ics_url;
    std::string sprite_dir = "./assets/sprites";
};

bool LoadConfig(const std::string& path, AppConfig* out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open config: " << path << "\n";
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse config: " << ex.what() << "\n";
        return false;
    }

    out->sync_interval_sec = j.value("sync_interval_sec", out->sync_interval_sec);
    out->time_window_days = j.value("time_window_days", out->time_window_days);
    out->idle_threshold_sec = j.value("idle_threshold_sec", out->idle_threshold_sec);
    out->auto_cycle_clock_sec = j.value("auto_cycle_clock_sec", out->auto_cycle_clock_sec);
    out->auto_cycle_calendar_sec = j.value("auto_cycle_calendar_sec", out->auto_cycle_calendar_sec);
    out->font_path = j.value("font_path", out->font_path);
    out->db_path = j.value("db_path", out->db_path);
    out->mock_mode = j.value("mock_mode", out->mock_mode);
    out->ics_url = j.value("ics_url", out->ics_url);
    out->sprite_dir = j.value("sprite_dir", out->sprite_dir);

    return true;
}

int main(int argc, char** argv) {
    std::string config_path = "config/config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    AppConfig config;
    if (!LoadConfig(config_path, &config)) {
        return 1;
    }

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

    CalendarSyncService sync_service(sync_config);
    sync_service.Start();

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

    TTF_Font* font_time = TTF_OpenFont(config.font_path.c_str(), 96);
    TTF_Font* font_date = TTF_OpenFont(config.font_path.c_str(), 20);
    TTF_Font* font_info = TTF_OpenFont(config.font_path.c_str(), 18);
    TTF_Font* font_header = TTF_OpenFont(config.font_path.c_str(), 22);
    TTF_Font* font_day = TTF_OpenFont(config.font_path.c_str(), 18);
    TTF_Font* font_agenda = TTF_OpenFont(config.font_path.c_str(), 18);

    if (!font_time || !font_date || !font_info || !font_header || !font_day || !font_agenda) {
        std::cerr << "Failed to load font: " << config.font_path << "\n";
        if (font_time) TTF_CloseFont(font_time);
        if (font_date) TTF_CloseFont(font_date);
        if (font_info) TTF_CloseFont(font_info);
        if (font_header) TTF_CloseFont(font_header);
        if (font_day) TTF_CloseFont(font_day);
        if (font_agenda) TTF_CloseFont(font_agenda);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    {
        ClockView clock_view(renderer, font_time, font_date, font_info, &store, config.sprite_dir);
        CalendarView calendar_view(renderer, font_header, font_day, font_agenda, &store);

        enum class ViewMode { Clock, Calendar };
        ViewMode current_view = ViewMode::Clock;

        auto last_input = std::chrono::steady_clock::now();
        bool auto_cycle = false;
        auto auto_cycle_start = last_input;
        bool capture_next_frame = false;

        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    running = false;
                } else if (ev.type == SDL_KEYDOWN) {
                    last_input = std::chrono::steady_clock::now();
                    auto_cycle = false;

                    switch (ev.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_SPACE:
                            current_view = (current_view == ViewMode::Clock) ? ViewMode::Calendar : ViewMode::Clock;
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
            if (!auto_cycle && idle_sec >= config.idle_threshold_sec) {
                auto_cycle = true;
                auto_cycle_start = now;
                current_view = ViewMode::Clock;
            }

            if (auto_cycle) {
                int cycle_total = config.auto_cycle_clock_sec + config.auto_cycle_calendar_sec;
                if (cycle_total > 0) {
                    int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - auto_cycle_start).count());
                    int mod = elapsed % cycle_total;
                    current_view = (mod < config.auto_cycle_clock_sec) ? ViewMode::Clock : ViewMode::Calendar;
                }
            }

            SDL_SetRenderDrawColor(renderer, 238, 236, 232, 255);
            SDL_RenderClear(renderer);

            int w = 0, h = 0;
            SDL_GetRendererOutputSize(renderer, &w, &h);

            if (current_view == ViewMode::Clock) {
                clock_view.Render(w, h);
            } else {
                calendar_view.Render(w, h);
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

    sync_service.Stop();

    TTF_CloseFont(font_time);
    TTF_CloseFont(font_date);
    TTF_CloseFont(font_info);
    TTF_CloseFont(font_header);
    TTF_CloseFont(font_day);
    TTF_CloseFont(font_agenda);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();

    return 0;
}
