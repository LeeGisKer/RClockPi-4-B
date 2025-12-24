#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <cstdint>

class EventStore;

class CalendarView {
public:
    CalendarView(SDL_Renderer* renderer, TTF_Font* header_font, TTF_Font* day_font, TTF_Font* agenda_font, EventStore* store);

    void Render(int width, int height);
    void MoveSelectionDays(int delta);
    void MoveMonth(int delta_months);
    void JumpToToday();

private:
    SDL_Renderer* renderer_;
    TTF_Font* header_font_;
    TTF_Font* day_font_;
    TTF_Font* agenda_font_;
    EventStore* store_;
    int64_t selected_ts_;
};
