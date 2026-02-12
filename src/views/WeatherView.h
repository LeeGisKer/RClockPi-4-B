#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class EventStore;

class WeatherView {
public:
    WeatherView(SDL_Renderer* renderer,
                TTF_Font* title_font,
                TTF_Font* body_font,
                TTF_Font* temp_font,
                EventStore* store,
                const std::string& sprite_dir);
    ~WeatherView();

    void Render(int width, int height);

private:
    struct CachedText {
        std::string text;
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        SDL_Color color{ 0, 0, 0, 0 };
    };

    struct SpriteTexture {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };

    struct HourlyEntry {
        std::string time_label;
        std::string temp_label;
        int code = -1;
        bool is_day = true;
    };

    struct DailyEntry {
        std::string day_label;
        std::string temp_label;
        int code = -1;
    };

    void UpdateCache(int width, int height, int64_t now_ts);
    void UpdateText(CachedText& cache, TTF_Font* font, const std::string& text, SDL_Color color);
    void ClearCache();
    void LoadSprites();
    void ClearSprites();
    bool DrawWeatherSprite(int code, bool is_day, const SDL_Rect& area);

    SDL_Renderer* renderer_;
    TTF_Font* title_font_;
    TTF_Font* body_font_;
    TTF_Font* temp_font_;
    EventStore* store_;
    std::string sprite_dir_;

    bool sprites_loaded_ = false;
    std::unordered_map<std::string, SpriteTexture> sprites_;

    int last_width_ = 0;
    int last_height_ = 0;
    int64_t last_minute_ = -1;

    std::string last_status_;
    std::string last_temp_c_;
    std::string last_summary_;
    std::string last_wind_kmh_;
    std::string last_error_;
    std::string last_weather_code_;
    std::string last_weather_is_day_;
    std::string last_hourly_json_;
    std::string last_daily_json_;
    std::string last_sync_ts_;

    int current_code_ = -1;
    bool current_is_day_ = true;
    std::vector<HourlyEntry> hourly_entries_;
    std::vector<DailyEntry> daily_entries_;

    CachedText title_text_;
    CachedText status_text_;
    CachedText temp_text_;
    CachedText summary_text_;
    CachedText detail_text_;
    CachedText hourly_title_text_;
    CachedText weekly_title_text_;
    CachedText hourly_empty_text_;
    CachedText daily_empty_text_;

    std::vector<CachedText> hourly_time_texts_;
    std::vector<CachedText> hourly_temp_texts_;
    std::vector<CachedText> daily_day_texts_;
    std::vector<CachedText> daily_temp_texts_;
};
