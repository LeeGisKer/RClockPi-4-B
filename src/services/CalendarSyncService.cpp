#include "services/CalendarSyncService.h"

#include "auth/OAuthTokenStore.h"
#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {

struct HttpResponse {
    long code = 0;
    std::string body;
};

size_t WriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), total);
    return total;
}

std::string UrlEncode(CURL* curl, const std::string& value) {
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!escaped) {
        return "";
    }
    std::string out = escaped;
    curl_free(escaped);
    return out;
}

bool HttpPostForm(CURL* curl, const std::string& url, const std::string& form_body, HttpResponse* out) {
    out->body.clear();
    out->code = 0;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rpi-calendar/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "HTTP POST failed: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->code);
    return true;
}

bool HttpGet(CURL* curl, const std::string& url, const std::vector<std::string>& headers, HttpResponse* out) {
    out->body.clear();
    out->code = 0;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rpi-calendar/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl);
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    if (res != CURLE_OK) {
        std::cerr << "HTTP GET failed: " << curl_easy_strerror(res) << "\n";
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->code);
    return true;
}

time_t TimegmPortable(std::tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

bool ParseIcsInt(const std::string& text, size_t start, size_t len, int* out) {
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

std::string ToUpper(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string ToLower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string IcsUnescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\' && i + 1 < value.size()) {
            char next = value[i + 1];
            if (next == 'n' || next == 'N') {
                out.push_back('\n');
            } else if (next == '\\') {
                out.push_back('\\');
            } else if (next == ';') {
                out.push_back(';');
            } else if (next == ',') {
                out.push_back(',');
            } else {
                out.push_back(next);
            }
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
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

bool LooksLikeUrl(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

bool ParseIcsDate(const std::string& value, time_t* out) {
    if (value.size() < 8) {
        return false;
    }
    int year = 0, month = 0, day = 0;
    if (!ParseIcsInt(value, 0, 4, &year) ||
        !ParseIcsInt(value, 4, 2, &month) ||
        !ParseIcsInt(value, 6, 2, &day)) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    time_t local = std::mktime(&tm);
    if (local == static_cast<time_t>(-1)) {
        return false;
    }
    *out = local;
    return true;
}

bool ParseIcsDateTime(const std::string& value, time_t* out, bool* is_utc) {
    std::string v = value;
    bool utc = false;
    if (!v.empty() && (v.back() == 'Z' || v.back() == 'z')) {
        utc = true;
        v.pop_back();
    }
    if (v.size() != 15 || v[8] != 'T') {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (!ParseIcsInt(v, 0, 4, &year) ||
        !ParseIcsInt(v, 4, 2, &month) ||
        !ParseIcsInt(v, 6, 2, &day) ||
        !ParseIcsInt(v, 9, 2, &hour) ||
        !ParseIcsInt(v, 11, 2, &min) ||
        !ParseIcsInt(v, 13, 2, &sec)) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    time_t ts = utc ? TimegmPortable(&tm) : std::mktime(&tm);
    if (ts == static_cast<time_t>(-1)) {
        return false;
    }
    if (is_utc) {
        *is_utc = utc;
    }
    *out = ts;
    return true;
}

bool SplitIcsLine(const std::string& line, std::string* name, std::string* params, std::string* value) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::string left = line.substr(0, colon);
    *value = line.substr(colon + 1);
    size_t semi = left.find(';');
    if (semi == std::string::npos) {
        *name = ToUpper(left);
        params->clear();
    } else {
        *name = ToUpper(left.substr(0, semi));
        *params = ToUpper(left.substr(semi + 1));
    }
    return true;
}

std::vector<std::string> UnfoldIcsLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    std::string current;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            current += line.substr(1);
        } else {
            if (!current.empty()) {
                lines.push_back(current);
            }
            current = line;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

bool FetchIcsEvents(CURL* curl, const SyncConfig& config, EventStore* store) {
    if (config.ics_url.empty()) {
        std::cerr << "ICS URL is empty.\n";
        return false;
    }
    HttpResponse resp;
    if (!HttpGet(curl, config.ics_url, {}, &resp)) {
        return false;
    }
    if (resp.code != 200) {
        std::cerr << "ICS fetch failed (HTTP " << resp.code << ")\n";
        return false;
    }

    auto lines = UnfoldIcsLines(resp.body);
    bool in_event = false;
    EventRecord ev;
    bool has_start = false;
    bool has_end = false;
    bool start_is_date = false;
    bool end_is_date = false;
    int64_t updated_ts = 0;

    int64_t now_ts = TimeUtil::NowTs();
    int64_t window_end = now_ts + static_cast<int64_t>(config.time_window_days) * 24 * 60 * 60;

    for (const auto& line : lines) {
        std::string name;
        std::string params;
        std::string value;
        if (!SplitIcsLine(line, &name, &params, &value)) {
            continue;
        }

        if (name == "BEGIN" && ToUpper(value) == "VEVENT") {
            in_event = true;
            ev = EventRecord{};
            ev.calendar_id = "ics";
            ev.status = "confirmed";
            has_start = false;
            has_end = false;
            start_is_date = false;
            end_is_date = false;
            updated_ts = 0;
            continue;
        }

        if (name == "END" && ToUpper(value) == "VEVENT") {
            if (in_event && has_start && !ev.id.empty()) {
                if (ev.title.empty()) {
                    ev.title = "(No title)";
                }

                if (!has_end) {
                    if (ev.all_day) {
                        ev.end_ts = ev.start_ts + 24 * 60 * 60 - 1;
                    } else {
                        ev.end_ts = ev.start_ts;
                    }
                } else if (ev.all_day && end_is_date) {
                    ev.end_ts = ev.end_ts - 1;
                }

                if (ev.end_ts < ev.start_ts) {
                    ev.end_ts = ev.start_ts;
                }

                ev.updated_ts = updated_ts > 0 ? updated_ts : now_ts;

                if (ev.end_ts >= now_ts && ev.start_ts <= window_end) {
                    store->UpsertEvent(ev);
                }
            }
            in_event = false;
            continue;
        }

        if (!in_event) {
            continue;
        }

        std::string unescaped = IcsUnescape(value);

        if (name == "UID") {
            ev.id = unescaped;
        } else if (name == "SUMMARY") {
            ev.title = unescaped;
        } else if (name == "LOCATION") {
            ev.location = unescaped;
        } else if (name == "STATUS") {
            ev.status = ToLower(unescaped);
        } else if (name == "DTSTART") {
            bool value_is_date = params.find("VALUE=DATE") != std::string::npos || unescaped.size() == 8;
            if (value_is_date) {
                time_t ts = 0;
                if (ParseIcsDate(unescaped, &ts)) {
                    ev.start_ts = static_cast<int64_t>(ts);
                    ev.all_day = true;
                    has_start = true;
                    start_is_date = true;
                }
            } else {
                time_t ts = 0;
                if (ParseIcsDateTime(unescaped, &ts, nullptr)) {
                    ev.start_ts = static_cast<int64_t>(ts);
                    ev.all_day = false;
                    has_start = true;
                    start_is_date = false;
                }
            }
        } else if (name == "DTEND") {
            bool value_is_date = params.find("VALUE=DATE") != std::string::npos || unescaped.size() == 8;
            if (value_is_date) {
                time_t ts = 0;
                if (ParseIcsDate(unescaped, &ts)) {
                    ev.end_ts = static_cast<int64_t>(ts);
                    has_end = true;
                    end_is_date = true;
                }
            } else {
                time_t ts = 0;
                if (ParseIcsDateTime(unescaped, &ts, nullptr)) {
                    ev.end_ts = static_cast<int64_t>(ts);
                    has_end = true;
                    end_is_date = false;
                }
            }
        } else if (name == "DTSTAMP" || name == "LAST-MODIFIED") {
            time_t ts = 0;
            if (ParseIcsDateTime(unescaped, &ts, nullptr)) {
                updated_ts = static_cast<int64_t>(ts);
            }
        }
    }

    return true;
}

bool RefreshAccessToken(CURL* curl, const SyncConfig& config, TokenInfo* token) {
    if (config.client_id.empty() || config.client_secret.empty() || token->refresh_token.empty()) {
        std::cerr << "Missing OAuth client_id, client_secret, or refresh_token.\n";
        return false;
    }

    std::ostringstream form;
    form << "client_id=" << UrlEncode(curl, config.client_id)
         << "&client_secret=" << UrlEncode(curl, config.client_secret)
         << "&refresh_token=" << UrlEncode(curl, token->refresh_token)
         << "&grant_type=refresh_token";

    HttpResponse resp;
    if (!HttpPostForm(curl, "https://oauth2.googleapis.com/token", form.str(), &resp)) {
        return false;
    }

    if (resp.code != 200) {
        std::cerr << "Token refresh failed (HTTP " << resp.code << ")\n";
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) {
        std::cerr << "Token refresh response parse failed.\n";
        return false;
    }

    token->access_token = j.value("access_token", "");
    token->token_type = j.value("token_type", "Bearer");
    int expires_in = j.value("expires_in", 0);
    token->expiry_ts = TimeUtil::NowTs() + std::max(0, expires_in - 30);

    if (token->access_token.empty()) {
        std::cerr << "Token refresh response missing access_token.\n";
        return false;
    }

    return OAuthTokenStore::SaveToFile(config.token_path, *token);
}

bool ParseEventItem(const nlohmann::json& item, const std::string& calendar_id, EventRecord* out) {
    if (!item.contains("id")) {
        return false;
    }

    out->id = item.value("id", "");
    out->calendar_id = calendar_id;
    out->title = item.value("summary", "(No title)");
    out->location = item.value("location", "");
    out->status = item.value("status", "");

    std::string updated = item.value("updated", "");
    time_t updated_ts = 0;
    if (!updated.empty()) {
        TimeUtil::ParseRfc3339(updated, &updated_ts);
    }
    out->updated_ts = updated_ts;

    if (!item.contains("start")) {
        return false;
    }

    const auto& start = item["start"];
    const auto& end = item.contains("end") ? item["end"] : nlohmann::json{};

    out->all_day = false;
    if (start.contains("dateTime")) {
        std::string start_dt = start.value("dateTime", "");
        time_t start_ts = 0;
        if (!TimeUtil::ParseRfc3339(start_dt, &start_ts)) {
            return false;
        }
        out->start_ts = static_cast<int64_t>(start_ts);
        if (end.contains("dateTime")) {
            std::string end_dt = end.value("dateTime", "");
            time_t end_ts = 0;
            if (TimeUtil::ParseRfc3339(end_dt, &end_ts)) {
                out->end_ts = static_cast<int64_t>(end_ts);
            } else {
                out->end_ts = out->start_ts;
            }
        } else {
            out->end_ts = out->start_ts;
        }
    } else if (start.contains("date")) {
        std::string start_date = start.value("date", "");
        time_t start_ts = 0;
        if (!TimeUtil::ParseDateLocal(start_date, &start_ts)) {
            return false;
        }
        out->start_ts = start_ts;
        out->all_day = true;
        if (end.contains("date")) {
            std::string end_date = end.value("date", "");
            time_t end_ts = 0;
            if (TimeUtil::ParseDateLocal(end_date, &end_ts)) {
                out->end_ts = end_ts;
            } else {
                out->end_ts = start_ts + 24 * 60 * 60;
            }
        } else {
            out->end_ts = start_ts + 24 * 60 * 60;
        }
    } else {
        return false;
    }

    if (out->end_ts < out->start_ts) {
        out->end_ts = out->start_ts;
    }

    return true;
}

bool FetchCalendarEvents(CURL* curl, const SyncConfig& config, const std::string& calendar_id, const std::string& access_token, EventStore* store, bool* unauthorized) {
    if (unauthorized) {
        *unauthorized = false;
    }
    std::string time_min = TimeUtil::ToRfc3339Utc(TimeUtil::NowTs());
    std::string time_max = TimeUtil::ToRfc3339Utc(TimeUtil::NowTs() + config.time_window_days * 24 * 60 * 60);

    std::string base = "https://www.googleapis.com/calendar/v3/calendars/" + UrlEncode(curl, calendar_id) + "/events";
    std::string page_token;

    while (true) {
        std::ostringstream url;
        url << base << "?singleEvents=true&orderBy=startTime&maxResults=2500"
            << "&timeMin=" << UrlEncode(curl, time_min)
            << "&timeMax=" << UrlEncode(curl, time_max)
            << "&showDeleted=true";
        if (!page_token.empty()) {
            url << "&pageToken=" << UrlEncode(curl, page_token);
        }

        std::vector<std::string> headers;
        headers.push_back("Authorization: Bearer " + access_token);

        HttpResponse resp;
        if (!HttpGet(curl, url.str(), headers, &resp)) {
            return false;
        }
        if (resp.code == 401) {
            if (unauthorized) {
                *unauthorized = true;
            }
            return false;
        }
        if (resp.code != 200) {
            std::cerr << "Events fetch failed (HTTP " << resp.code << ")\n";
            return false;
        }

        nlohmann::json j = nlohmann::json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) {
            std::cerr << "Events response parse failed.\n";
            return false;
        }

        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& item : j["items"]) {
                EventRecord ev;
                if (!ParseEventItem(item, calendar_id, &ev)) {
                    continue;
                }
                store->UpsertEvent(ev);
            }
        }

        if (j.contains("nextPageToken")) {
            page_token = j.value("nextPageToken", "");
            if (page_token.empty()) {
                break;
            }
        } else {
            break;
        }
    }

    return true;
}

} // namespace

CalendarSyncService::CalendarSyncService(const SyncConfig& config) : config_(config) {}

CalendarSyncService::~CalendarSyncService() {
    Stop();
}

void CalendarSyncService::Start() {
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread(&CalendarSyncService::Run, this);
}

void CalendarSyncService::Stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool CalendarSyncService::IsRunning() const {
    return running_.load();
}

void CalendarSyncService::Run() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        std::cerr << "libcurl global init failed\n";
        return;
    }

    EventStore store(config_.db_path);
    if (!store.Open()) {
        std::cerr << "CalendarSyncService: failed to open DB\n";
        curl_global_cleanup();
        return;
    }

    bool seeded = false;
    while (running_) {
        int64_t now_ts = TimeUtil::NowTs();
        bool ok = false;

        if (config_.mock_mode) {
            if (!seeded) {
                ok = store.InsertSampleEvents(now_ts);
                seeded = true;
            } else {
                ok = true;
            }
            store.SetMeta("last_sync_status", "mock");
        } else {
            ok = SyncOnce(&store);
            store.SetMeta("last_sync_status", ok ? "online" : "offline");
        }

        store.SetMeta("last_sync_ts", std::to_string(now_ts));
        if (!ok) {
            store.SetMeta("last_sync_error", "sync failed");
        } else {
            store.SetMeta("last_sync_error", "");
        }

        for (int i = 0; i < config_.sync_interval_sec && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    curl_global_cleanup();
}

bool CalendarSyncService::SyncOnce(EventStore* store) {
    if (!store) {
        return false;
    }

    std::string ics_url = Trim(config_.ics_url);
    if (!ics_url.empty()) {
        if (!LooksLikeUrl(ics_url)) {
            std::cerr << "ICS URL is not a valid http(s) URL.\n";
            return false;
        }
        SyncConfig ics_config = config_;
        ics_config.ics_url = ics_url;
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "libcurl init failed\n";
            return false;
        }
        bool ok = FetchIcsEvents(curl, ics_config, store);
        curl_easy_cleanup(curl);
        return ok;
    }

    if (config_.client_id.empty() || config_.client_secret.empty()) {
        std::cerr << "OAuth not configured. Set ics_url or provide client_id/client_secret.\n";
        return false;
    }

    TokenInfo token;
    if (!OAuthTokenStore::LoadFromFile(config_.token_path, &token)) {
        return false;
    }

    if (token.refresh_token.empty()) {
        std::cerr << "Refresh token missing in token.json\n";
        return false;
    }

    int64_t now_ts = TimeUtil::NowTs();
    bool need_refresh = token.access_token.empty() || token.expiry_ts <= now_ts + 60;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "libcurl init failed\n";
        return false;
    }

    bool ok = true;
    if (need_refresh) {
        ok = RefreshAccessToken(curl, config_, &token);
    }

    if (ok) {
        for (const auto& calendar_id : config_.calendar_ids) {
            bool unauthorized = false;
            if (!FetchCalendarEvents(curl, config_, calendar_id, token.access_token, store, &unauthorized)) {
                if (unauthorized) {
                    if (!RefreshAccessToken(curl, config_, &token)) {
                        ok = false;
                        break;
                    }
                    if (!FetchCalendarEvents(curl, config_, calendar_id, token.access_token, store, nullptr)) {
                        ok = false;
                        break;
                    }
                } else {
                    ok = false;
                    break;
                }
            }
        }
    }

    curl_easy_cleanup(curl);
    return ok;
}
