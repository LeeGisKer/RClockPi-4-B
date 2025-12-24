#include "views/ClockView.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

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
        return label + " (never)";
    }
    int64_t last_ts = std::stoll(ts_str);
    int64_t minutes = (now_ts - last_ts) / 60;
    if (minutes < 0) {
        minutes = 0;
    }
    return label + " (" + std::to_string(minutes) + "m)";
}

} // namespace

ClockView::ClockView(SDL_Renderer* renderer, TTF_Font* time_font, TTF_Font* date_font, TTF_Font* info_font, EventStore* store)
    : renderer_(renderer), time_font_(time_font), date_font_(date_font), info_font_(info_font), store_(store) {}

void ClockView::Render(int width, int height) {
    int64_t now_ts = TimeUtil::NowTs();
    std::string time_text = TimeUtil::FormatTimeHHMM(now_ts);
    std::string date_text = TimeUtil::FormatDateLine(now_ts);

    SDL_Color fg = { 40, 40, 40, 255 };
    SDL_Color dim = { 90, 90, 90, 255 };
    SDL_Color line = { 170, 170, 170, 255 };

    int time_w = 0, time_h = 0;
    SDL_Texture* time_tex = RenderText(renderer_, time_font_, time_text, fg, &time_w, &time_h);

    int date_w = 0, date_h = 0;
    SDL_Texture* date_tex = RenderText(renderer_, date_font_, date_text, dim, &date_w, &date_h);

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

    int margin = std::max(16, width / 25);
    SDL_Rect panel{ margin, margin, width - 2 * margin, height - 2 * margin };
    int top_h = static_cast<int>(panel.h * 0.6f);
    int bottom_h = panel.h - top_h;
    int left_w = static_cast<int>(panel.w * 0.28f);
    int center_w = static_cast<int>(panel.w * 0.42f);
    int right_w = panel.w - left_w - center_w;
    int top_y = panel.y;
    int divider_y = panel.y + top_h;

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawRect(renderer_, &panel);
    SDL_RenderDrawLine(renderer_, panel.x, divider_y, panel.x + panel.w, divider_y);
    SDL_RenderDrawLine(renderer_, panel.x + left_w, top_y, panel.x + left_w, divider_y);
    SDL_RenderDrawLine(renderer_, panel.x + left_w + center_w, top_y, panel.x + left_w + center_w, divider_y);

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    int icon_cx = panel.x + left_w / 2;
    int icon_cy = top_y + top_h / 2;
    int radius = std::min(left_w, top_h) / 4;
    DrawClockIcon(renderer_, icon_cx, icon_cy, radius);

    if (date_tex) {
        int date_x = panel.x + left_w + (center_w - date_w) / 2;
        int date_y = top_y + 18;
        SDL_Rect dst{ date_x, date_y, date_w, date_h };
        SDL_RenderCopy(renderer_, date_tex, nullptr, &dst);
        SDL_DestroyTexture(date_tex);
    }

    if (time_tex) {
        int time_x = panel.x + left_w + (center_w - time_w) / 2;
        int time_y = top_y + (top_h - time_h) / 2 + 10;
        SDL_Rect dst{ time_x, time_y, time_w, time_h };
        SDL_RenderCopy(renderer_, time_tex, nullptr, &dst);
        SDL_DestroyTexture(time_tex);
    }

    int right_x = panel.x + left_w + center_w + 16;
    int right_y = top_y + 18;
    int right_max_w = right_w - 28;
    std::string next_short = has_next ? (TimeUtil::FormatTimeHHMM(next_event.start_ts) + " " + next_event.title) : "No events";
    next_short = TruncateText(info_font_, next_short, right_max_w);
    std::string sync_text = SyncStatusLabel(store_, now_ts);
    std::string today_count = "Today: " + std::to_string(static_cast<int>(today_events.size())) + " events";

    std::vector<std::string> right_lines = { "Next up", next_short, today_count, sync_text };
    int line_y = right_y;
    for (size_t i = 0; i < right_lines.size(); ++i) {
        SDL_Color color = (i == 1) ? fg : dim;
        int w = 0, h = 0;
        SDL_Texture* tex = RenderText(renderer_, info_font_, right_lines[i], color, &w, &h);
        if (tex) {
            SDL_Rect dst{ right_x, line_y, w, h };
            SDL_RenderCopy(renderer_, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
        line_y += h + 6;
    }

    int footer_h = 28;
    int grid_h = bottom_h - footer_h;
    int grid_y = divider_y;
    int grid_x = panel.x;
    int grid_w = panel.w;
    int row_h = grid_h / 2;
    int col_w = grid_w / 2;

    SDL_RenderDrawLine(renderer_, grid_x + col_w, grid_y, grid_x + col_w, grid_y + grid_h);
    SDL_RenderDrawLine(renderer_, grid_x, grid_y + row_h, grid_x + grid_w, grid_y + row_h);

    auto draw_cell = [&](int col, int row, const std::string& label, const std::string& value) {
        int cell_x = grid_x + col * col_w + 16;
        int cell_y = grid_y + row * row_h + 10;
        int w = 0, h = 0;
        SDL_Texture* label_tex = RenderText(renderer_, info_font_, label, dim, &w, &h);
        if (label_tex) {
            SDL_Rect dst{ cell_x, cell_y, w, h };
            SDL_RenderCopy(renderer_, label_tex, nullptr, &dst);
            SDL_DestroyTexture(label_tex);
        }
        int value_w = 0, value_h = 0;
        SDL_Texture* value_tex = RenderText(renderer_, info_font_, value, fg, &value_w, &value_h);
        if (value_tex) {
            SDL_Rect dst{ cell_x, cell_y + h + 6, value_w, value_h };
            SDL_RenderCopy(renderer_, value_tex, nullptr, &dst);
            SDL_DestroyTexture(value_tex);
        }
    };

    draw_cell(0, 0, "Today", std::to_string(static_cast<int>(today_events.size())) + " events");
    draw_cell(1, 0, "Tomorrow", std::to_string(static_cast<int>(tomorrow_events.size())) + " events");
    draw_cell(0, 1, "All day", std::to_string(all_day_today) + " today");
    draw_cell(1, 1, "Remaining", std::to_string(remaining_today) + " today");

    int next_w = 0, next_h = 0;
    std::string footer_text = TruncateText(info_font_, next_line, panel.w - 24);
    SDL_Texture* next_tex = RenderText(renderer_, info_font_, footer_text, dim, &next_w, &next_h);
    if (next_tex) {
        SDL_Rect dst{ panel.x + 12, panel.y + panel.h - next_h - 8, next_w, next_h };
        SDL_RenderCopy(renderer_, next_tex, nullptr, &dst);
        SDL_DestroyTexture(next_tex);
    }
}
