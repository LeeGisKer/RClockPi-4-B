#include "views/ClockView.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ClockLayout {
    int margin = 0;
    SDL_Rect panel{};
    int top_h = 0;
    int bottom_h = 0;
    int left_w = 0;
    int center_w = 0;
    int right_w = 0;
    int top_y = 0;
    int divider_y = 0;
    int right_x = 0;
    int right_y = 0;
    int right_max_w = 0;
    int footer_max_w = 0;
    int footer_h = 0;
    int grid_y = 0;
    int grid_h = 0;
    int row_h = 0;
    int col_w = 0;
};

ClockLayout ComputeLayout(int width, int height) {
    ClockLayout layout;
    layout.margin = std::max(16, width / 25);
    layout.panel = { layout.margin, layout.margin, width - 2 * layout.margin, height - 2 * layout.margin };
    layout.top_h = static_cast<int>(layout.panel.h * 0.6f);
    layout.bottom_h = layout.panel.h - layout.top_h;
    layout.left_w = static_cast<int>(layout.panel.w * 0.28f);
    layout.center_w = static_cast<int>(layout.panel.w * 0.42f);
    layout.right_w = layout.panel.w - layout.left_w - layout.center_w;
    layout.top_y = layout.panel.y;
    layout.divider_y = layout.panel.y + layout.top_h;
    layout.right_x = layout.panel.x + layout.left_w + layout.center_w + 16;
    layout.right_y = layout.top_y + 18;
    layout.right_max_w = layout.right_w - 28;
    layout.footer_h = 28;
    layout.footer_max_w = layout.panel.w - 24;
    layout.grid_h = layout.bottom_h - layout.footer_h;
    layout.grid_y = layout.divider_y;
    layout.row_h = layout.grid_h / 2;
    layout.col_w = layout.panel.w / 2;
    return layout;
}

SDL_Texture* RenderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, SDL_Color color, int* w, int* h) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) {
        std::cerr << "TTF_RenderUTF8_Blended failed: " << TTF_GetError() << "\n";
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
    if (!tex) {
        std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << "\n";
        SDL_FreeSurface(surface);
        return nullptr;
    }
    *w = surface->w;
    *h = surface->h;
    SDL_FreeSurface(surface);
    return tex;
}

std::string TruncateText(TTF_Font* font, const std::string& text, int max_width) {
    int w = 0;
    int h = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &w, &h) == 0 && w <= max_width) {
        return text;
    }
    const std::string ellipsis = "...";
    for (size_t len = text.size(); len > 0; --len) {
        std::string candidate = text.substr(0, len) + ellipsis;
        if (TTF_SizeUTF8(font, candidate.c_str(), &w, &h) == 0 && w <= max_width) {
            return candidate;
        }
    }
    return ellipsis;
}

void DrawCircle(SDL_Renderer* renderer, int cx, int cy, int radius) {
    int x = radius;
    int y = 0;
    int err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(renderer, cx + x, cy + y);
        SDL_RenderDrawPoint(renderer, cx + y, cy + x);
        SDL_RenderDrawPoint(renderer, cx - y, cy + x);
        SDL_RenderDrawPoint(renderer, cx - x, cy + y);
        SDL_RenderDrawPoint(renderer, cx - x, cy - y);
        SDL_RenderDrawPoint(renderer, cx - y, cy - x);
        SDL_RenderDrawPoint(renderer, cx + y, cy - x);
        SDL_RenderDrawPoint(renderer, cx + x, cy - y);
        if (err <= 0) {
            ++y;
            err += 2 * y + 1;
        }
        if (err > 0) {
            --x;
            err -= 2 * x + 1;
        }
    }
}

void DrawClockIcon(SDL_Renderer* renderer, int cx, int cy, int radius) {
    DrawCircle(renderer, cx, cy, radius);
    SDL_RenderDrawLine(renderer, cx, cy, cx, cy - radius + 6);
    SDL_RenderDrawLine(renderer, cx, cy, cx + radius - 6, cy);
}

std::string SyncStatusLabel(EventStore* store, int64_t now_ts) {
    if (!store) {
        return "Offline";
    }
    std::string status = store->GetMeta("last_sync_status");
    std::string ts_str = store->GetMeta("last_sync_ts");
    std::string label = status.empty() ? "Offline" : status;
    if (label == "online") {
        label = "Online";
    } else if (label == "offline") {
        label = "Offline";
    } else if (label == "mock") {
        label = "Mock";
    }
    if (ts_str.empty()) {
        std::string err = store->GetMeta("last_sync_error");
        if (!err.empty()) {
            return label + " (" + err + ")";
        }
        return label + " (never)";
    }
    int64_t last_ts = 0;
    try {
        last_ts = std::stoll(ts_str);
    } catch (const std::exception&) {
        return label + " (unknown)";
    }
    int64_t minutes = (now_ts - last_ts) / 60;
    if (minutes < 0) {
        minutes = 0;
    }
    return label + " (" + std::to_string(minutes) + "m)";
}

} // namespace

ClockView::ClockView(SDL_Renderer* renderer, TTF_Font* time_font, TTF_Font* date_font, TTF_Font* info_font, EventStore* store)
    : renderer_(renderer), time_font_(time_font), date_font_(date_font), info_font_(info_font), store_(store) {}

ClockView::~ClockView() {
    ClearCache();
}

void ClockView::UpdateText(CachedText& cache, TTF_Font* font, const std::string& text, SDL_Color color) {
    if (cache.texture && cache.text == text && cache.color.r == color.r && cache.color.g == color.g && cache.color.b == color.b && cache.color.a == color.a) {
        return;
    }
    if (cache.texture) {
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
    }
    cache.text = text;
    cache.color = color;
    cache.texture = RenderText(renderer_, font, text, color, &cache.w, &cache.h);
}

void ClockView::ClearCache() {
    auto destroy = [](CachedText& cache) {
        if (cache.texture) {
            SDL_DestroyTexture(cache.texture);
            cache.texture = nullptr;
        }
    };
    destroy(time_text_);
    destroy(date_text_);
    destroy(footer_text_);
    for (auto& item : right_texts_) {
        destroy(item);
    }
    for (auto& item : cell_labels_) {
        destroy(item);
    }
    for (auto& item : cell_values_) {
        destroy(item);
    }
}

void ClockView::UpdateCache(int width, int height, int64_t now_ts) {
    int64_t minute = now_ts / 60;
    bool size_changed = width != last_width_ || height != last_height_;
    if (minute == last_minute_ && !size_changed) {
        return;
    }
    last_minute_ = minute;
    last_width_ = width;
    last_height_ = height;

    ClockLayout layout = ComputeLayout(width, height);

    SDL_Color fg = { 40, 40, 40, 255 };
    SDL_Color dim = { 90, 90, 90, 255 };

    UpdateText(time_text_, time_font_, TimeUtil::FormatTimeHHMM(now_ts), fg);
    UpdateText(date_text_, date_font_, TimeUtil::FormatDateLine(now_ts), dim);

    std::string next_line;
    EventRecord next_event;
    bool has_next = store_ && store_->GetNextEventAfter(now_ts, &next_event);
    if (has_next && next_event.start_ts <= TimeUtil::EndOfDay(now_ts)) {
        int minutes = static_cast<int>((next_event.start_ts - now_ts) / 60);
        if (minutes < 0) {
            minutes = 0;
        }
        next_line = "Next: " + TimeUtil::FormatTimeHHMM(next_event.start_ts) + " - " + next_event.title + " (in " + std::to_string(minutes) + "m)";
    } else {
        next_line = "No more events today";
    }
    std::string footer_text = TruncateText(info_font_, next_line, layout.footer_max_w);
    UpdateText(footer_text_, info_font_, footer_text, dim);

    std::vector<EventRecord> today_events = store_ ? store_->GetEventsForDay(now_ts) : std::vector<EventRecord>();
    std::vector<EventRecord> tomorrow_events = store_ ? store_->GetEventsForDay(now_ts + 24 * 60 * 60) : std::vector<EventRecord>();
    int all_day_today = 0;
    int remaining_today = 0;
    for (const auto& ev : today_events) {
        if (ev.all_day) {
            all_day_today++;
        }
        if (ev.start_ts >= now_ts) {
            remaining_today++;
        }
    }

    std::string next_short = has_next ? (TimeUtil::FormatTimeHHMM(next_event.start_ts) + " " + next_event.title) : "No events";
    next_short = TruncateText(info_font_, next_short, layout.right_max_w);

    std::array<std::string, 4> right_lines = {
        "Next up",
        next_short,
        "Today: " + std::to_string(static_cast<int>(today_events.size())) + " events",
        SyncStatusLabel(store_, now_ts)
    };

    for (size_t i = 0; i < right_lines.size(); ++i) {
        SDL_Color color = (i == 1) ? fg : dim;
        UpdateText(right_texts_[i], info_font_, right_lines[i], color);
    }

    std::array<std::string, 4> labels = { "Today", "Tomorrow", "All day", "Remaining" };
    std::array<std::string, 4> values = {
        std::to_string(static_cast<int>(today_events.size())) + " events",
        std::to_string(static_cast<int>(tomorrow_events.size())) + " events",
        std::to_string(all_day_today) + " today",
        std::to_string(remaining_today) + " today"
    };

    for (size_t i = 0; i < labels.size(); ++i) {
        UpdateText(cell_labels_[i], info_font_, labels[i], dim);
        UpdateText(cell_values_[i], info_font_, values[i], fg);
    }
}

void ClockView::Render(int width, int height) {
    int64_t now_ts = TimeUtil::NowTs();
    UpdateCache(width, height, now_ts);

    ClockLayout layout = ComputeLayout(width, height);

    SDL_Color line = { 170, 170, 170, 255 };

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawRect(renderer_, &layout.panel);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.divider_y, layout.panel.x + layout.panel.w, layout.divider_y);
    SDL_RenderDrawLine(renderer_, layout.panel.x + layout.left_w, layout.top_y, layout.panel.x + layout.left_w, layout.divider_y);
    SDL_RenderDrawLine(renderer_, layout.panel.x + layout.left_w + layout.center_w, layout.top_y, layout.panel.x + layout.left_w + layout.center_w, layout.divider_y);

    int icon_cx = layout.panel.x + layout.left_w / 2;
    int icon_cy = layout.top_y + layout.top_h / 2;
    int radius = std::min(layout.left_w, layout.top_h) / 4;
    DrawClockIcon(renderer_, icon_cx, icon_cy, radius);

    if (date_text_.texture) {
        int date_x = layout.panel.x + layout.left_w + (layout.center_w - date_text_.w) / 2;
        int date_y = layout.top_y + 18;
        SDL_Rect dst{ date_x, date_y, date_text_.w, date_text_.h };
        SDL_RenderCopy(renderer_, date_text_.texture, nullptr, &dst);
    }

    if (time_text_.texture) {
        int time_x = layout.panel.x + layout.left_w + (layout.center_w - time_text_.w) / 2;
        int time_y = layout.top_y + (layout.top_h - time_text_.h) / 2 + 10;
        SDL_Rect dst{ time_x, time_y, time_text_.w, time_text_.h };
        SDL_RenderCopy(renderer_, time_text_.texture, nullptr, &dst);
    }

    int line_y = layout.right_y;
    for (const auto& item : right_texts_) {
        if (!item.texture) {
            continue;
        }
        SDL_Rect dst{ layout.right_x, line_y, item.w, item.h };
        SDL_RenderCopy(renderer_, item.texture, nullptr, &dst);
        line_y += item.h + 6;
    }

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawLine(renderer_, layout.panel.x + layout.col_w, layout.grid_y, layout.panel.x + layout.col_w, layout.grid_y + layout.grid_h);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.grid_y + layout.row_h, layout.panel.x + layout.panel.w, layout.grid_y + layout.row_h);

    auto draw_cell = [&](int col, int row, const CachedText& label, const CachedText& value) {
        int cell_x = layout.panel.x + col * layout.col_w + 16;
        int cell_y = layout.grid_y + row * layout.row_h + 10;
        if (label.texture) {
            SDL_Rect dst{ cell_x, cell_y, label.w, label.h };
            SDL_RenderCopy(renderer_, label.texture, nullptr, &dst);
        }
        if (value.texture) {
            SDL_Rect dst{ cell_x, cell_y + label.h + 6, value.w, value.h };
            SDL_RenderCopy(renderer_, value.texture, nullptr, &dst);
        }
    };

    draw_cell(0, 0, cell_labels_[0], cell_values_[0]);
    draw_cell(1, 0, cell_labels_[1], cell_values_[1]);
    draw_cell(0, 1, cell_labels_[2], cell_values_[2]);
    draw_cell(1, 1, cell_labels_[3], cell_values_[3]);

    if (footer_text_.texture) {
        SDL_Rect dst{ layout.panel.x + 12, layout.panel.y + layout.panel.h - footer_text_.h - 8, footer_text_.w, footer_text_.h };
        SDL_RenderCopy(renderer_, footer_text_.texture, nullptr, &dst);
    }
}
