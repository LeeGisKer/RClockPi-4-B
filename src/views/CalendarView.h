#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class EventStore;

class CalendarView {
public:
    CalendarView(SDL_Renderer* renderer, TTF_Font* header_font, TTF_Font* day_font, TTF_Font* agenda_font, EventStore* store);
    ~CalendarView();

    void Render(int width, int height);
    void MoveSelectionDays(int delta);
    void MoveMonth(int delta_months);
    void JumpToToday();

private:
    struct CachedText {
        std::string text;
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        SDL_Color color{ 0, 0, 0, 0 };
    };

    void UpdateCache(int width, int height, int64_t now_ts);
    void UpdateText(CachedText& cache, TTF_Font* font, const std::string& text, SDL_Color color);
    void ClearCache();
    void RebuildDayTextures(int days_in_month, SDL_Color color);

    SDL_Renderer* renderer_;
    TTF_Font* header_font_;
    TTF_Font* day_font_;
    TTF_Font* agenda_font_;
    EventStore* store_;
    int64_t selected_ts_;

    int last_width_ = 0;
    int last_height_ = 0;
    int last_year_ = -1;
    int last_month_ = -1;
    int last_day_ = -1;
    int64_t last_minute_ = -1;

    std::vector<CachedText> day_texts_;
    std::map<int, int> event_days_cache_;
    CachedText month_text_;
    CachedText sync_text_;
    CachedText agenda_title_;
    std::vector<CachedText> agenda_lines_;
    CachedText more_text_;
    int remaining_count_ = 0;
};
