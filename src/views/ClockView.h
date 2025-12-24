#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <array>
#include <string>
#include <vector>

class EventStore;

class ClockView {
public:
    ClockView(SDL_Renderer* renderer, TTF_Font* time_font, TTF_Font* date_font, TTF_Font* info_font, EventStore* store, const std::string& sprite_dir);
    ~ClockView();
    void Render(int width, int height);

private:
    enum class SpriteKind {
        Midnight = 0,
        Sunrise = 1,
        Sun = 2,
        Sunset = 3,
        Moon = 4,
        Count = 5
    };

    struct SpriteTexture {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };

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
    void LoadSprites();
    void ClearSprites();
    bool DrawSpriteForHour(int hour, const SDL_Rect& area);
    SpriteKind SpriteForHour(int hour) const;

    SDL_Renderer* renderer_;
    TTF_Font* time_font_;
    TTF_Font* date_font_;
    TTF_Font* info_font_;
    EventStore* store_;
    std::string sprite_dir_;
    bool sprites_loaded_ = false;
    std::array<SpriteTexture, static_cast<size_t>(SpriteKind::Count)> sprites_{};

    int last_width_ = 0;
    int last_height_ = 0;
    int64_t last_minute_ = -1;

    CachedText time_text_;
    CachedText date_text_;
    CachedText footer_text_;
    std::array<CachedText, 4> right_texts_;
    std::array<CachedText, 4> cell_labels_;
    std::array<CachedText, 4> cell_values_;
};
