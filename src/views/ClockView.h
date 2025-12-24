#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

class EventStore;

class ClockView {
public:
    ClockView(SDL_Renderer* renderer, TTF_Font* time_font, TTF_Font* date_font, TTF_Font* info_font, EventStore* store);
    void Render(int width, int height);

private:
    SDL_Renderer* renderer_;
    TTF_Font* time_font_;
    TTF_Font* date_font_;
    TTF_Font* info_font_;
    EventStore* store_;
};
