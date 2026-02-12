#include "views/WeatherView.h"

#include "db/EventStore.h"

#include <SDL_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

struct WeatherLayout {
    int margin = 0;
    SDL_Rect panel{};
    SDL_Rect top{};
    SDL_Rect hourly{};
    SDL_Rect weekly{};
    int hourly_columns = 8;
};

WeatherLayout ComputeLayout(int width, int height) {
    WeatherLayout layout;
    layout.margin = std::max(12, width / 32);
    layout.panel = { layout.margin, layout.margin, width - 2 * layout.margin, height - 2 * layout.margin };

    int top_h = static_cast<int>(layout.panel.h * 0.36f);
    int hourly_h = static_cast<int>(layout.panel.h * 0.30f);
    int weekly_h = layout.panel.h - top_h - hourly_h;

    layout.top = { layout.panel.x, layout.panel.y, layout.panel.w, top_h };
    layout.hourly = { layout.panel.x, layout.panel.y + top_h, layout.panel.w, hourly_h };
    layout.weekly = { layout.panel.x, layout.panel.y + top_h + hourly_h, layout.panel.w, weekly_h };
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

std::string JoinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) {
        return file;
    }
    char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + file;
    }
    return dir + "/" + file;
}

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

bool ParseInt(const std::string& text, size_t start, size_t len, int* out) {
    if (start + len > text.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = text[start + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

std::string FormatHourLabel(const std::string& iso_datetime) {
    int hour = 0;
    if (iso_datetime.size() >= 13 && ParseInt(iso_datetime, 11, 2, &hour)) {
        int hour12 = hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }
        return std::to_string(hour12) + (hour < 12 ? "AM" : "PM");
    }
    return "--";
}

std::string WeekdayShortFromDate(const std::string& date_iso) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (date_iso.size() < 10 ||
        !ParseInt(date_iso, 0, 4, &year) ||
        !ParseInt(date_iso, 5, 2, &month) ||
        !ParseInt(date_iso, 8, 2, &day)) {
        return "--";
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    std::mktime(&tm);

    static const std::array<const char*, 7> kWeekdays = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (tm.tm_wday < 0 || tm.tm_wday >= 7) {
        return "--";
    }
    return kWeekdays[tm.tm_wday];
}

std::string FormatDecimal1(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value;
    return out.str();
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

std::string WeatherSpriteKey(int code, bool is_day) {
    if (code == 0) {
        return is_day ? "clear" : "clear_night";
    }
    if (code == 1) {
        return "mostly_clear";
    }
    if (code == 2) {
        return "partly_cloudy";
    }
    if (code == 3) {
        return "overcast";
    }
    if (code == 45 || code == 48) {
        return "fog";
    }
    if (code >= 51 && code <= 57) {
        return "drizzle";
    }
    if ((code >= 61 && code <= 67) || code == 77) {
        return "rain";
    }
    if (code >= 71 && code <= 75) {
        return "snow";
    }
    if (code >= 80 && code <= 86) {
        return "showers";
    }
    if (code >= 95 && code <= 99) {
        return "thunder";
    }
    return "unknown";
}

std::string BuildWeatherStatusLine(const std::string& status,
                                   const std::string& sync_ts,
                                   const std::string& error,
                                   int64_t now_ts) {
    if (status == "disabled") {
        return "Weather off";
    }
    if (status == "config") {
        return "Weather setup needed";
    }

    std::string base = status.empty() ? "Weather" : status;
    if (base == "online") {
        base = "Online";
    } else if (base == "offline") {
        base = "Offline";
    }

    if (sync_ts.empty()) {
        if (!error.empty()) {
            return base + " (" + error + ")";
        }
        return base;
    }

    int64_t ts = 0;
    try {
        ts = std::stoll(sync_ts);
    } catch (const std::exception&) {
        return base;
    }
    int64_t min_ago = (now_ts - ts) / 60;
    if (min_ago < 0) {
        min_ago = 0;
    }
    if (status == "online") {
        return "Updated " + std::to_string(min_ago) + "m ago";
    }
    return base + " (" + std::to_string(min_ago) + "m)";
}

} // namespace

WeatherView::WeatherView(SDL_Renderer* renderer,
                         TTF_Font* title_font,
                         TTF_Font* body_font,
                         TTF_Font* temp_font,
                         EventStore* store,
                         const std::string& sprite_dir)
    : renderer_(renderer),
      title_font_(title_font),
      body_font_(body_font),
      temp_font_(temp_font),
      store_(store),
      sprite_dir_(sprite_dir) {
    LoadSprites();
}

WeatherView::~WeatherView() {
    ClearCache();
    ClearSprites();
}

void WeatherView::UpdateText(CachedText& cache, TTF_Font* font, const std::string& text, SDL_Color color) {
    if (text.empty()) {
        if (cache.texture) {
            SDL_DestroyTexture(cache.texture);
            cache.texture = nullptr;
        }
        cache.text.clear();
        cache.w = 0;
        cache.h = 0;
        cache.color = color;
        return;
    }
    if (cache.texture &&
        cache.text == text &&
        cache.color.r == color.r &&
        cache.color.g == color.g &&
        cache.color.b == color.b &&
        cache.color.a == color.a) {
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

void WeatherView::ClearCache() {
    auto destroy = [](CachedText& text) {
        if (text.texture) {
            SDL_DestroyTexture(text.texture);
            text.texture = nullptr;
        }
    };

    destroy(title_text_);
    destroy(status_text_);
    destroy(temp_text_);
    destroy(summary_text_);
    destroy(detail_text_);
    destroy(hourly_title_text_);
    destroy(weekly_title_text_);
    destroy(hourly_empty_text_);
    destroy(daily_empty_text_);

    for (auto& text : hourly_time_texts_) {
        destroy(text);
    }
    hourly_time_texts_.clear();
    for (auto& text : hourly_temp_texts_) {
        destroy(text);
    }
    hourly_temp_texts_.clear();
    for (auto& text : daily_day_texts_) {
        destroy(text);
    }
    daily_day_texts_.clear();
    for (auto& text : daily_temp_texts_) {
        destroy(text);
    }
    daily_temp_texts_.clear();
}

void WeatherView::LoadSprites() {
    sprites_loaded_ = false;
    sprites_.clear();
    if (sprite_dir_.empty()) {
        return;
    }

    const std::array<std::pair<const char*, const char*>, 12> files = {{
        { "clear", "clear.png" },
        { "clear_night", "clear_night.png" },
        { "mostly_clear", "mostly_clear.png" },
        { "partly_cloudy", "partly_cloudy.png" },
        { "overcast", "overcast.png" },
        { "fog", "fog.png" },
        { "drizzle", "drizzle.png" },
        { "rain", "rain.png" },
        { "snow", "snow.png" },
        { "showers", "showers.png" },
        { "thunder", "thunder.png" },
        { "unknown", "unknown.png" }
    }};

    for (const auto& item : files) {
        std::string path = JoinPath(sprite_dir_, item.second);
        SDL_Surface* surface = IMG_Load(path.c_str());
        if (!surface) {
            continue;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        if (!texture) {
            SDL_FreeSurface(surface);
            continue;
        }
        SpriteTexture sprite;
        sprite.texture = texture;
        sprite.w = surface->w;
        sprite.h = surface->h;
        sprites_[item.first] = sprite;
        SDL_FreeSurface(surface);
        sprites_loaded_ = true;
    }
}

void WeatherView::ClearSprites() {
    for (auto& [_, sprite] : sprites_) {
        if (sprite.texture) {
            SDL_DestroyTexture(sprite.texture);
            sprite.texture = nullptr;
        }
    }
    sprites_.clear();
    sprites_loaded_ = false;
}

bool WeatherView::DrawWeatherSprite(int code, bool is_day, const SDL_Rect& area) {
    if (!sprites_loaded_) {
        return false;
    }
    std::string key = WeatherSpriteKey(code, is_day);
    auto it = sprites_.find(key);
    if (it == sprites_.end() || !it->second.texture) {
        it = sprites_.find("unknown");
        if (it == sprites_.end() || !it->second.texture) {
            return false;
        }
    }

    const SpriteTexture& sprite = it->second;
    int max_w = std::max(1, area.w);
    int max_h = std::max(1, area.h);
    float scale = std::min(static_cast<float>(max_w) / std::max(1, sprite.w),
                           static_cast<float>(max_h) / std::max(1, sprite.h));
    int draw_w = static_cast<int>(std::round(sprite.w * scale));
    int draw_h = static_cast<int>(std::round(sprite.h * scale));
    SDL_Rect dst{
        area.x + (area.w - draw_w) / 2,
        area.y + (area.h - draw_h) / 2,
        draw_w,
        draw_h
    };
    SDL_RenderCopy(renderer_, sprite.texture, nullptr, &dst);
    return true;
}

void WeatherView::UpdateCache(int width, int height, int64_t now_ts) {
    std::string status = store_ ? store_->GetMeta("weather_status") : "";
    std::string temp_c = store_ ? store_->GetMeta("weather_temp_c") : "";
    std::string summary = store_ ? store_->GetMeta("weather_summary") : "";
    std::string wind_kmh = store_ ? store_->GetMeta("weather_wind_kmh") : "";
    std::string error = store_ ? store_->GetMeta("weather_error") : "";
    std::string weather_code = store_ ? store_->GetMeta("weather_code") : "";
    std::string weather_is_day = store_ ? store_->GetMeta("weather_is_day") : "";
    std::string hourly_json = store_ ? store_->GetMeta("weather_hourly_json") : "";
    std::string daily_json = store_ ? store_->GetMeta("weather_daily_json") : "";
    std::string sync_ts = store_ ? store_->GetMeta("weather_last_sync_ts") : "";

    bool size_changed = width != last_width_ || height != last_height_;
    int64_t minute = now_ts / 60;
    bool minute_changed = minute != last_minute_;
    bool data_changed = status != last_status_ ||
                        temp_c != last_temp_c_ ||
                        summary != last_summary_ ||
                        wind_kmh != last_wind_kmh_ ||
                        error != last_error_ ||
                        weather_code != last_weather_code_ ||
                        weather_is_day != last_weather_is_day_ ||
                        hourly_json != last_hourly_json_ ||
                        daily_json != last_daily_json_ ||
                        sync_ts != last_sync_ts_;

    if (!size_changed && !minute_changed && !data_changed) {
        return;
    }

    last_width_ = width;
    last_height_ = height;
    last_minute_ = minute;
    last_status_ = status;
    last_temp_c_ = temp_c;
    last_summary_ = summary;
    last_wind_kmh_ = wind_kmh;
    last_error_ = error;
    last_weather_code_ = weather_code;
    last_weather_is_day_ = weather_is_day;
    last_hourly_json_ = hourly_json;
    last_daily_json_ = daily_json;
    last_sync_ts_ = sync_ts;

    hourly_entries_.clear();
    daily_entries_.clear();

    nlohmann::json hourly = nlohmann::json::parse(hourly_json, nullptr, false);
    if (!hourly.is_discarded() && hourly.is_array()) {
        for (const auto& item : hourly) {
            if (!item.is_object()) {
                continue;
            }
            HourlyEntry entry;
            entry.time_label = FormatHourLabel(item.value("time", ""));
            entry.code = item.value("code", -1);
            entry.is_day = item.value("is_day", 1) == 1;
            if (item.contains("temp_c") && item["temp_c"].is_number()) {
                entry.temp_label = FormatDecimal1(item.value("temp_c", 0.0)) + " C";
            } else {
                entry.temp_label = Trim(item.value("temp_c", std::string{}));
                if (!entry.temp_label.empty() && entry.temp_label.find('C') == std::string::npos) {
                    entry.temp_label += " C";
                }
            }
            if (entry.temp_label.empty()) {
                entry.temp_label = "--";
            }
            hourly_entries_.push_back(std::move(entry));
            if (hourly_entries_.size() >= 8) {
                break;
            }
        }
    }

    nlohmann::json daily = nlohmann::json::parse(daily_json, nullptr, false);
    if (!daily.is_discarded() && daily.is_array()) {
        for (const auto& item : daily) {
            if (!item.is_object()) {
                continue;
            }
            DailyEntry entry;
            entry.day_label = WeekdayShortFromDate(item.value("date", ""));
            double max_c = item.value("max_c", std::nan(""));
            double min_c = item.value("min_c", std::nan(""));
            if (std::isfinite(max_c) && std::isfinite(min_c)) {
                entry.temp_label = "H " + FormatDecimal1(max_c) + " / L " + FormatDecimal1(min_c);
            } else {
                entry.temp_label = "--";
            }
            entry.code = item.value("code", -1);
            daily_entries_.push_back(std::move(entry));
            if (daily_entries_.size() >= 7) {
                break;
            }
        }
    }

    current_code_ = -1;
    current_is_day_ = true;
    try {
        current_code_ = std::stoi(weather_code);
    } catch (const std::exception&) {
        current_code_ = -1;
    }
    current_is_day_ = (weather_is_day != "0");

    SDL_Color fg = { 28, 28, 28, 255 };
    SDL_Color dim = { 110, 110, 110, 255 };

    UpdateText(title_text_, title_font_, "Weather", fg);
    UpdateText(status_text_, body_font_, BuildWeatherStatusLine(status, sync_ts, error, now_ts), dim);
    UpdateText(temp_text_, temp_font_, temp_c.empty() ? "--" : (temp_c + " C"), fg);
    UpdateText(summary_text_, body_font_, summary.empty() ? "No weather data" : summary, fg);

    std::string detail = wind_kmh.empty() ? "" : ("Wind " + wind_kmh + " km/h");
    if (!error.empty() && status == "offline") {
        if (!detail.empty()) {
            detail += "  ";
        }
        detail += "Error: " + error;
    } else if (status == "offline") {
        if (!detail.empty()) {
            detail += "  ";
        }
        detail += "Using cached forecast";
    }
    UpdateText(detail_text_, body_font_, detail, dim);

    UpdateText(hourly_title_text_, body_font_, "Hourly", dim);
    UpdateText(weekly_title_text_, body_font_, "7-Day", dim);
    UpdateText(hourly_empty_text_, body_font_, "No hourly forecast yet", dim);
    UpdateText(daily_empty_text_, body_font_, "No daily forecast yet", dim);

    auto reset_list = [](std::vector<CachedText>& list) {
        for (auto& item : list) {
            if (item.texture) {
                SDL_DestroyTexture(item.texture);
                item.texture = nullptr;
            }
        }
        list.clear();
    };
    reset_list(hourly_time_texts_);
    reset_list(hourly_temp_texts_);
    reset_list(daily_day_texts_);
    reset_list(daily_temp_texts_);

    hourly_time_texts_.assign(hourly_entries_.size(), CachedText{});
    hourly_temp_texts_.assign(hourly_entries_.size(), CachedText{});
    for (size_t i = 0; i < hourly_entries_.size(); ++i) {
        UpdateText(hourly_time_texts_[i], body_font_, hourly_entries_[i].time_label, dim);
        UpdateText(hourly_temp_texts_[i], body_font_, hourly_entries_[i].temp_label, fg);
    }

    daily_day_texts_.assign(daily_entries_.size(), CachedText{});
    daily_temp_texts_.assign(daily_entries_.size(), CachedText{});
    for (size_t i = 0; i < daily_entries_.size(); ++i) {
        UpdateText(daily_day_texts_[i], body_font_, daily_entries_[i].day_label, fg);
        UpdateText(daily_temp_texts_[i], body_font_, daily_entries_[i].temp_label, dim);
    }
}

void WeatherView::Render(int width, int height) {
    int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
    UpdateCache(width, height, now_ts);

    WeatherLayout layout = ComputeLayout(width, height);
    SDL_Color line = { 200, 200, 200, 255 };
    SDL_Color dim = { 110, 110, 110, 255 };

    SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
    SDL_RenderDrawRect(renderer_, &layout.panel);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.hourly.y, layout.panel.x + layout.panel.w, layout.hourly.y);
    SDL_RenderDrawLine(renderer_, layout.panel.x, layout.weekly.y, layout.panel.x + layout.panel.w, layout.weekly.y);

    if (title_text_.texture) {
        SDL_Rect dst{ layout.top.x + 14, layout.top.y + 10, title_text_.w, title_text_.h };
        SDL_RenderCopy(renderer_, title_text_.texture, nullptr, &dst);
    }
    if (status_text_.texture) {
        SDL_Rect dst{
            layout.top.x + layout.top.w - status_text_.w - 14,
            layout.top.y + 12,
            status_text_.w,
            status_text_.h
        };
        SDL_RenderCopy(renderer_, status_text_.texture, nullptr, &dst);
    }

    int icon_size = std::max(56, layout.top.h - 56);
    SDL_Rect top_icon{
        layout.top.x + 16,
        layout.top.y + 34,
        std::max(24, std::min(icon_size, layout.top.w / 4)),
        std::max(24, std::min(icon_size, layout.top.h - 44))
    };
    if (!DrawWeatherSprite(current_code_, current_is_day_, top_icon)) {
        SDL_SetRenderDrawColor(renderer_, dim.r, dim.g, dim.b, 255);
        SDL_RenderDrawRect(renderer_, &top_icon);
    }

    int info_x = top_icon.x + top_icon.w + 18;
    int info_max_w = std::max(32, layout.top.x + layout.top.w - info_x - 14);

    if (temp_text_.texture) {
        SDL_Rect dst{ info_x, layout.top.y + 30, temp_text_.w, temp_text_.h };
        SDL_RenderCopy(renderer_, temp_text_.texture, nullptr, &dst);
    }
    if (summary_text_.texture) {
        std::string summary = TruncateText(body_font_, summary_text_.text, info_max_w);
        if (summary != summary_text_.text) {
            SDL_Color fg = { 28, 28, 28, 255 };
            CachedText tmp;
            UpdateText(tmp, body_font_, summary, fg);
            if (tmp.texture) {
                SDL_Rect dst{ info_x, layout.top.y + 78, tmp.w, tmp.h };
                SDL_RenderCopy(renderer_, tmp.texture, nullptr, &dst);
                SDL_DestroyTexture(tmp.texture);
            }
        } else {
            SDL_Rect dst{ info_x, layout.top.y + 78, summary_text_.w, summary_text_.h };
            SDL_RenderCopy(renderer_, summary_text_.texture, nullptr, &dst);
        }
    }
    if (detail_text_.texture) {
        std::string detail = TruncateText(body_font_, detail_text_.text, info_max_w);
        if (detail != detail_text_.text) {
            CachedText tmp;
            UpdateText(tmp, body_font_, detail, dim);
            if (tmp.texture) {
                SDL_Rect dst{ info_x, layout.top.y + 104, tmp.w, tmp.h };
                SDL_RenderCopy(renderer_, tmp.texture, nullptr, &dst);
                SDL_DestroyTexture(tmp.texture);
            }
        } else {
            SDL_Rect dst{ info_x, layout.top.y + 104, detail_text_.w, detail_text_.h };
            SDL_RenderCopy(renderer_, detail_text_.texture, nullptr, &dst);
        }
    }

    if (hourly_title_text_.texture) {
        SDL_Rect dst{ layout.hourly.x + 14, layout.hourly.y + 8, hourly_title_text_.w, hourly_title_text_.h };
        SDL_RenderCopy(renderer_, hourly_title_text_.texture, nullptr, &dst);
    }

    SDL_Rect hourly_body{
        layout.hourly.x + 10,
        layout.hourly.y + 28,
        layout.hourly.w - 20,
        layout.hourly.h - 34
    };
    if (hourly_entries_.empty()) {
        if (hourly_empty_text_.texture) {
            SDL_Rect dst{
                hourly_body.x + (hourly_body.w - hourly_empty_text_.w) / 2,
                hourly_body.y + (hourly_body.h - hourly_empty_text_.h) / 2,
                hourly_empty_text_.w,
                hourly_empty_text_.h
            };
            SDL_RenderCopy(renderer_, hourly_empty_text_.texture, nullptr, &dst);
        }
    } else {
        int cols = std::min<int>(8, hourly_entries_.size());
        int cell_w = hourly_body.w / std::max(1, cols);
        for (int i = 0; i < cols; ++i) {
            SDL_Rect cell{
                hourly_body.x + i * cell_w,
                hourly_body.y,
                (i == cols - 1) ? (hourly_body.w - cell_w * i) : cell_w,
                hourly_body.h
            };
            if (i > 0) {
                SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
                SDL_RenderDrawLine(renderer_, cell.x, cell.y + 4, cell.x, cell.y + cell.h - 4);
            }
            if (i < static_cast<int>(hourly_time_texts_.size()) && hourly_time_texts_[i].texture) {
                SDL_Rect dst{
                    cell.x + (cell.w - hourly_time_texts_[i].w) / 2,
                    cell.y + 2,
                    hourly_time_texts_[i].w,
                    hourly_time_texts_[i].h
                };
                SDL_RenderCopy(renderer_, hourly_time_texts_[i].texture, nullptr, &dst);
            }

            SDL_Rect icon_rect{
                cell.x + (cell.w - 34) / 2,
                cell.y + 22,
                34,
                std::max(18, cell.h - 54)
            };
            DrawWeatherSprite(hourly_entries_[i].code, hourly_entries_[i].is_day, icon_rect);

            if (i < static_cast<int>(hourly_temp_texts_.size()) && hourly_temp_texts_[i].texture) {
                SDL_Rect dst{
                    cell.x + (cell.w - hourly_temp_texts_[i].w) / 2,
                    cell.y + cell.h - hourly_temp_texts_[i].h - 2,
                    hourly_temp_texts_[i].w,
                    hourly_temp_texts_[i].h
                };
                SDL_RenderCopy(renderer_, hourly_temp_texts_[i].texture, nullptr, &dst);
            }
        }
    }

    if (weekly_title_text_.texture) {
        SDL_Rect dst{ layout.weekly.x + 14, layout.weekly.y + 8, weekly_title_text_.w, weekly_title_text_.h };
        SDL_RenderCopy(renderer_, weekly_title_text_.texture, nullptr, &dst);
    }

    SDL_Rect weekly_body{
        layout.weekly.x + 10,
        layout.weekly.y + 28,
        layout.weekly.w - 20,
        layout.weekly.h - 32
    };
    if (daily_entries_.empty()) {
        if (daily_empty_text_.texture) {
            SDL_Rect dst{
                weekly_body.x + (weekly_body.w - daily_empty_text_.w) / 2,
                weekly_body.y + (weekly_body.h - daily_empty_text_.h) / 2,
                daily_empty_text_.w,
                daily_empty_text_.h
            };
            SDL_RenderCopy(renderer_, daily_empty_text_.texture, nullptr, &dst);
        }
    } else {
        int rows = std::min<int>(7, daily_entries_.size());
        int row_h = std::max(18, weekly_body.h / std::max(1, rows));
        for (int i = 0; i < rows; ++i) {
            SDL_Rect row{
                weekly_body.x,
                weekly_body.y + i * row_h,
                weekly_body.w,
                (i == rows - 1) ? (weekly_body.h - row_h * i) : row_h
            };

            if (i > 0) {
                SDL_SetRenderDrawColor(renderer_, line.r, line.g, line.b, 255);
                SDL_RenderDrawLine(renderer_, row.x, row.y, row.x + row.w, row.y);
            }

            if (i < static_cast<int>(daily_day_texts_.size()) && daily_day_texts_[i].texture) {
                SDL_Rect dst{
                    row.x + 4,
                    row.y + (row.h - daily_day_texts_[i].h) / 2,
                    daily_day_texts_[i].w,
                    daily_day_texts_[i].h
                };
                SDL_RenderCopy(renderer_, daily_day_texts_[i].texture, nullptr, &dst);
            }

            SDL_Rect icon_rect{ row.x + 72, row.y + 3, 20, std::max(12, row.h - 6) };
            DrawWeatherSprite(daily_entries_[i].code, true, icon_rect);

            if (i < static_cast<int>(daily_temp_texts_.size()) && daily_temp_texts_[i].texture) {
                SDL_Rect dst{
                    row.x + row.w - daily_temp_texts_[i].w - 4,
                    row.y + (row.h - daily_temp_texts_[i].h) / 2,
                    daily_temp_texts_[i].w,
                    daily_temp_texts_[i].h
                };
                SDL_RenderCopy(renderer_, daily_temp_texts_[i].texture, nullptr, &dst);
            }
        }
    }
}
