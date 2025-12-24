#include "views/CalendarView.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <algorithm>
#include <map>
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

    int64_t last_ts = std::stoll(ts_str);
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
    std::tm now_tm = TimeUtil::LocalTime(now_ts);
    std::tm sel_tm = TimeUtil::LocalTime(selected_ts_);

    int year = sel_tm.tm_year + 1900;
    int month = sel_tm.tm_mon + 1;

    SDL_Color fg = { 40, 40, 40, 255 };
    SDL_Color dim = { 90, 90, 90, 255 };
    SDL_Color line = { 170, 170, 170, 255 };
    SDL_Color accent = { 60, 60, 60, 255 };
    SDL_Color highlight = { 210, 210, 210, 255 };

    int margin = std::max(16, width / 25);
    SDL_Rect panel{ margin, margin, width - 2 * margin, height - 2 * margin };
    int top_bar_h = static_cast<int>(panel.h * 0.12f);
    int grid_h = static_cast<int>(panel.h * 0.56f);
    int agenda_h = panel.h - top_bar_h - grid_h;

    std::string month_text = TimeUtil::FormatMonthYear(selected_ts_);
    int title_w = 0, title_h = 0;
    SDL_Texture* title_tex = RenderText(renderer_, header_font_, month_text, fg, &title_w, &title_h);

    std::string sync_text = SyncStatusText(store_, now_ts);
    int sync_w = 0, sync_h = 0;
    SDL_Texture* sync_tex = RenderText(renderer_, agenda_font_, sync_text, dim, &sync_w, &sync_h);

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawRect(renderer_, &panel);
    SDL_RenderDrawLine(renderer_, panel.x, panel.y + top_bar_h, panel.x + panel.w, panel.y + top_bar_h);

    if (title_tex) {
        SDL_Rect dst{ panel.x + 16, panel.y + (top_bar_h - title_h) / 2, title_w, title_h };
        SDL_RenderCopy(renderer_, title_tex, nullptr, &dst);
        SDL_DestroyTexture(title_tex);
    }
    if (sync_tex) {
        SDL_Rect dst{ panel.x + panel.w - sync_w - 16, panel.y + (top_bar_h - sync_h) / 2, sync_w, sync_h };
        SDL_RenderCopy(renderer_, sync_tex, nullptr, &dst);
        SDL_DestroyTexture(sync_tex);
    }

    int grid_y = panel.y + top_bar_h;
    int cell_w = panel.w / 7;
    int cell_h = grid_h / 6;

    auto event_days = store_ ? store_->GetEventDaysInMonth(year, month) : std::map<int, int>();

    int first_wday = TimeUtil::WeekdayIndex(year, month, 1);
    int days_in_month = TimeUtil::DaysInMonth(year, month);

    int day = 1;
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 7; ++col) {
            int cell_x = panel.x + col * cell_w;
            int cell_y = grid_y + row * cell_h;
            SDL_Rect cell{ cell_x, cell_y, cell_w, cell_h };
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

                std::string day_text = std::to_string(day);
                int day_w = 0, day_h = 0;
                SDL_Texture* day_tex = RenderText(renderer_, day_font_, day_text, fg, &day_w, &day_h);
                if (day_tex) {
                    SDL_Rect dst{ cell_x + 6, cell_y + 4, day_w, day_h };
                    SDL_RenderCopy(renderer_, day_tex, nullptr, &dst);
                    SDL_DestroyTexture(day_tex);
                }

                auto it = event_days.find(day);
                if (it != event_days.end()) {
                    SDL_SetRenderDrawColor(renderer_, accent.r, accent.g, accent.b, 255);
                    SDL_Rect dot{ cell_x + cell_w - 10, cell_y + cell_h - 10, 5, 5 };
                    SDL_RenderFillRect(renderer_, &dot);
                }

                day++;
            }
        }
    }

    int agenda_y = grid_y + grid_h;
    SDL_Rect agenda_rect{ panel.x, agenda_y, panel.w, agenda_h };
    SDL_SetRenderDrawColor(renderer_, 225, 222, 216, 255);
    SDL_RenderFillRect(renderer_, &agenda_rect);
    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawLine(renderer_, panel.x, agenda_y, panel.x + panel.w, agenda_y);

    std::vector<EventRecord> events = store_ ? store_->GetEventsForDay(selected_ts_) : std::vector<EventRecord>();

    std::string agenda_title = "Agenda - " + TimeUtil::FormatDateLine(selected_ts_);
    int title_w2 = 0, title_h2 = 0;
    SDL_Texture* agenda_tex = RenderText(renderer_, agenda_font_, agenda_title, dim, &title_w2, &title_h2);
    if (agenda_tex) {
        SDL_Rect dst{ panel.x + 16, agenda_y + 8, title_w2, title_h2 };
        SDL_RenderCopy(renderer_, agenda_tex, nullptr, &dst);
        SDL_DestroyTexture(agenda_tex);
    }

    int line_y = agenda_y + 8 + title_h2 + 6;
    int max_lines = 5;
    int shown = 0;
    for (const auto& ev : events) {
        if (shown >= max_lines) {
            break;
        }
        std::string time_label = ev.all_day ? "All day" : TimeUtil::FormatTimeHHMM(ev.start_ts);
        std::string line = time_label + "  " + ev.title;
        line = TruncateText(agenda_font_, line, panel.w - 32);
        int line_w = 0, line_h = 0;
        SDL_Texture* line_tex = RenderText(renderer_, agenda_font_, line, fg, &line_w, &line_h);
        if (line_tex) {
            SDL_Rect dst{ panel.x + 16, line_y, line_w, line_h };
            SDL_RenderCopy(renderer_, line_tex, nullptr, &dst);
            SDL_DestroyTexture(line_tex);
        }
        line_y += line_h + 6;
        shown++;
    }

    if (events.size() > static_cast<size_t>(max_lines)) {
        int remaining = static_cast<int>(events.size()) - max_lines;
        std::string more = "+" + std::to_string(remaining) + " more...";
        int more_w = 0, more_h = 0;
        SDL_Texture* more_tex = RenderText(renderer_, agenda_font_, more, dim, &more_w, &more_h);
        if (more_tex) {
            SDL_Rect dst{ panel.x + 16, line_y, more_w, more_h };
            SDL_RenderCopy(renderer_, more_tex, nullptr, &dst);
            SDL_DestroyTexture(more_tex);
        }
    }
}
