#include "views/CalendarView.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct CalendarLayout {
    int margin = 0;
    SDL_Rect panel{};
    int top_bar_h = 0;
    int grid_h = 0;
    int agenda_h = 0;
    int grid_y = 0;
    int cell_w = 0;
    int cell_h = 0;
    int agenda_y = 0;
    int agenda_max_w = 0;
};

CalendarLayout ComputeLayout(int width, int height) {
    CalendarLayout layout;
    layout.margin = std::max(16, width / 25);
    layout.panel = { layout.margin, layout.margin, width - 2 * layout.margin, height - 2 * layout.margin };
    layout.top_bar_h = static_cast<int>(layout.panel.h * 0.12f);
    layout.grid_h = static_cast<int>(layout.panel.h * 0.56f);
    layout.agenda_h = layout.panel.h - layout.top_bar_h - layout.grid_h;
    layout.grid_y = layout.panel.y + layout.top_bar_h;
    layout.cell_w = layout.panel.w / 7;
    layout.cell_h = layout.grid_h / 6;
    layout.agenda_y = layout.grid_y + layout.grid_h;
    layout.agenda_max_w = layout.panel.w - 32;
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

std::string SyncStatusText(EventStore* store, int64_t now_ts) {
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

CalendarView::CalendarView(SDL_Renderer* renderer, TTF_Font* header_font, TTF_Font* day_font, TTF_Font* agenda_font, EventStore* store)
    : renderer_(renderer), header_font_(header_font), day_font_(day_font), agenda_font_(agenda_font), store_(store) {
    selected_ts_ = TimeUtil::NowTs();
}

CalendarView::~CalendarView() {
    ClearCache();
}

void CalendarView::UpdateText(CachedText& cache, TTF_Font* font, const std::string& text, SDL_Color color) {
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

void CalendarView::ClearCache() {
    auto destroy = [](CachedText& cache) {
        if (cache.texture) {
            SDL_DestroyTexture(cache.texture);
            cache.texture = nullptr;
        }
    };

    destroy(month_text_);
    destroy(sync_text_);
    destroy(agenda_title_);
    destroy(more_text_);

    for (auto& item : day_texts_) {
        destroy(item);
    }
    day_texts_.clear();

    for (auto& item : agenda_lines_) {
        destroy(item);
    }
    agenda_lines_.clear();
}

void CalendarView::RebuildDayTextures(int days_in_month, SDL_Color color) {
    for (auto& item : day_texts_) {
        if (item.texture) {
            SDL_DestroyTexture(item.texture);
        }
    }
    day_texts_.assign(days_in_month, CachedText{});
    for (int day = 1; day <= days_in_month; ++day) {
        UpdateText(day_texts_[day - 1], day_font_, std::to_string(day), color);
    }
}

void CalendarView::UpdateCache(int width, int height, int64_t now_ts) {
    CalendarLayout layout = ComputeLayout(width, height);
    std::tm sel_tm = TimeUtil::LocalTime(selected_ts_);

    int year = sel_tm.tm_year + 1900;
    int month = sel_tm.tm_mon + 1;
    int day = sel_tm.tm_mday;

    bool size_changed = width != last_width_ || height != last_height_;
    bool month_changed = (year != last_year_ || month != last_month_);
    bool day_changed = (day != last_day_) || month_changed;
    int64_t minute = now_ts / 60;
    bool minute_changed = minute != last_minute_;

    SDL_Color fg = { 40, 40, 40, 255 };
    SDL_Color dim = { 90, 90, 90, 255 };

    if (month_changed || size_changed) {
        UpdateText(month_text_, header_font_, TimeUtil::FormatMonthYear(selected_ts_), fg);
        int days_in_month = TimeUtil::DaysInMonth(year, month);
        RebuildDayTextures(days_in_month, fg);
    }

    if (minute_changed || size_changed || month_changed) {
        UpdateText(sync_text_, agenda_font_, SyncStatusText(store_, now_ts), dim);
    }

    if (store_ && (minute_changed || month_changed)) {
        event_days_cache_ = store_->GetEventDaysInMonth(year, month);
    } else if (!store_) {
        event_days_cache_.clear();
    }

    if (day_changed || minute_changed || size_changed) {
        UpdateText(agenda_title_, agenda_font_, "Agenda - " + TimeUtil::FormatDateLine(selected_ts_), dim);

        for (auto& item : agenda_lines_) {
            if (item.texture) {
                SDL_DestroyTexture(item.texture);
            }
        }
        agenda_lines_.clear();
        remaining_count_ = 0;
        if (more_text_.texture) {
            SDL_DestroyTexture(more_text_.texture);
            more_text_.texture = nullptr;
            more_text_.text.clear();
        }

        std::vector<EventRecord> events = store_ ? store_->GetEventsForDay(selected_ts_) : std::vector<EventRecord>();
        int max_lines = 5;
        int shown = 0;
        for (const auto& ev : events) {
            if (shown >= max_lines) {
                break;
            }
            std::string time_label = ev.all_day ? "All day" : TimeUtil::FormatTimeHHMM(ev.start_ts);
            std::string line = time_label + "  " + ev.title;
            line = TruncateText(agenda_font_, line, layout.agenda_max_w);
            CachedText cache;
            UpdateText(cache, agenda_font_, line, fg);
            agenda_lines_.push_back(std::move(cache));
            shown++;
        }

        if (events.size() > static_cast<size_t>(max_lines)) {
            remaining_count_ = static_cast<int>(events.size()) - max_lines;
            std::string more = "+" + std::to_string(remaining_count_) + " more...";
            UpdateText(more_text_, agenda_font_, more, dim);
        }
    }

    last_width_ = width;
    last_height_ = height;
    last_year_ = year;
    last_month_ = month;
    last_day_ = day;
    last_minute_ = minute;
}

void CalendarView::MoveSelectionDays(int delta) {
    selected_ts_ += delta * 24 * 60 * 60;
}

void CalendarView::MoveMonth(int delta_months) {
    std::tm tm = TimeUtil::LocalTime(selected_ts_);
    int day = tm.tm_mday;
    tm.tm_mon += delta_months;
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    int max_day = TimeUtil::DaysInMonth(year, month);
    tm.tm_mday = std::min(day, max_day);
    selected_ts_ = std::mktime(&tm);
}

void CalendarView::JumpToToday() {
    selected_ts_ = TimeUtil::NowTs();
}

void CalendarView::Render(int width, int height) {
    int64_t now_ts = TimeUtil::NowTs();
    UpdateCache(width, height, now_ts);

    std::tm now_tm = TimeUtil::LocalTime(now_ts);
    std::tm sel_tm = TimeUtil::LocalTime(selected_ts_);

    int year = sel_tm.tm_year + 1900;
    int month = sel_tm.tm_mon + 1;

    CalendarLayout layout = ComputeLayout(width, height);

    SDL_Color line = { 170, 170, 170, 255 };
    SDL_Color accent = { 60, 60, 60, 255 };
    SDL_Color highlight = { 210, 210, 210, 255 };

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawRect(renderer_, &layout.panel);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.panel.y + layout.top_bar_h, layout.panel.x + layout.panel.w, layout.panel.y + layout.top_bar_h);

    if (month_text_.texture) {
        SDL_Rect dst{ layout.panel.x + 16, layout.panel.y + (layout.top_bar_h - month_text_.h) / 2, month_text_.w, month_text_.h };
        SDL_RenderCopy(renderer_, month_text_.texture, nullptr, &dst);
    }
    if (sync_text_.texture) {
        SDL_Rect dst{ layout.panel.x + layout.panel.w - sync_text_.w - 16, layout.panel.y + (layout.top_bar_h - sync_text_.h) / 2, sync_text_.w, sync_text_.h };
        SDL_RenderCopy(renderer_, sync_text_.texture, nullptr, &dst);
    }

    int first_wday = TimeUtil::WeekdayIndex(year, month, 1);
    int days_in_month = TimeUtil::DaysInMonth(year, month);

    int day = 1;
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 7; ++col) {
            int cell_x = layout.panel.x + col * layout.cell_w;
            int cell_y = layout.grid_y + row * layout.cell_h;
            SDL_Rect cell{ cell_x, cell_y, layout.cell_w, layout.cell_h };
            SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
            SDL_RenderDrawRect(renderer_, &cell);

            int index = row * 7 + col;
            if (index >= first_wday && day <= days_in_month) {
                bool is_today = (day == now_tm.tm_mday && month == (now_tm.tm_mon + 1) && year == (now_tm.tm_year + 1900));
                bool is_selected = (day == sel_tm.tm_mday && month == (sel_tm.tm_mon + 1) && year == (sel_tm.tm_year + 1900));

                if (is_selected) {
                    SDL_SetRenderDrawColor(renderer_, highlight.r, highlight.g, highlight.b, 255);
                    SDL_RenderFillRect(renderer_, &cell);
                    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
                    SDL_RenderDrawRect(renderer_, &cell);
                }

                if (is_today) {
                    SDL_SetRenderDrawColor(renderer_, accent.r, accent.g, accent.b, 255);
                    SDL_RenderDrawRect(renderer_, &cell);
                }

                if (day - 1 < static_cast<int>(day_texts_.size())) {
                    const auto& day_cache = day_texts_[day - 1];
                    if (day_cache.texture) {
                        SDL_Rect dst{ cell_x + 6, cell_y + 4, day_cache.w, day_cache.h };
                        SDL_RenderCopy(renderer_, day_cache.texture, nullptr, &dst);
                    }
                }

                auto it = event_days_cache_.find(day);
                if (it != event_days_cache_.end()) {
                    SDL_SetRenderDrawColor(renderer_, accent.r, accent.g, accent.b, 255);
                    SDL_Rect dot{ cell_x + layout.cell_w - 10, cell_y + layout.cell_h - 10, 5, 5 };
                    SDL_RenderFillRect(renderer_, &dot);
                }

                day++;
            }
        }
    }

    SDL_Rect agenda_rect{ layout.panel.x, layout.agenda_y, layout.panel.w, layout.agenda_h };
    SDL_SetRenderDrawColor(renderer_, 225, 222, 216, 255);
    SDL_RenderFillRect(renderer_, &agenda_rect);
    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.agenda_y, layout.panel.x + layout.panel.w, layout.agenda_y);

    int line_y = layout.agenda_y + 8;
    if (agenda_title_.texture) {
        SDL_Rect dst{ layout.panel.x + 16, line_y, agenda_title_.w, agenda_title_.h };
        SDL_RenderCopy(renderer_, agenda_title_.texture, nullptr, &dst);
        line_y += agenda_title_.h + 6;
    }

    for (const auto& line_cache : agenda_lines_) {
        if (!line_cache.texture) {
            continue;
        }
        SDL_Rect dst{ layout.panel.x + 16, line_y, line_cache.w, line_cache.h };
        SDL_RenderCopy(renderer_, line_cache.texture, nullptr, &dst);
        line_y += line_cache.h + 6;
    }

    if (remaining_count_ > 0 && more_text_.texture) {
        SDL_Rect dst{ layout.panel.x + 16, line_y, more_text_.w, more_text_.h };
        SDL_RenderCopy(renderer_, more_text_.texture, nullptr, &dst);
    }
}
