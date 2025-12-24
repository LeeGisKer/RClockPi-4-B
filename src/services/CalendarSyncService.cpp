#include "services/CalendarSyncService.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <curl/curl.h>
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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

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

bool FetchIcsEvents(CURL* curl, const SyncConfig& config, EventStore* store, int64_t sync_ts, std::string* error) {
    if (config.ics_url.empty()) {
        std::cerr << "ICS URL is empty.\n";
        if (error) {
            *error = "ics_url empty";
        }
        return false;
    }
    HttpResponse resp;
    if (!HttpGet(curl, config.ics_url, {}, &resp)) {
        if (error) {
            *error = "ics http failed";
        }
        return false;
    }
    if (resp.code != 200) {
        std::cerr << "ICS fetch failed (HTTP " << resp.code << ")\n";
        if (error) {
            *error = "ics http " + std::to_string(resp.code);
        }
        return false;
    }

    auto lines = UnfoldIcsLines(resp.body);
    bool in_event = false;
    EventRecord ev;
    bool has_start = false;
    bool has_end = false;
    bool start_is_date = false;
    bool end_is_date = false;
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

                ev.updated_ts = sync_ts;

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
            continue;
        }
    }

    store->DeleteStaleInWindow("ics", now_ts, window_end, sync_ts);
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
        std::string error;

        if (config_.mock_mode) {
            if (!seeded) {
                ok = store.InsertSampleEvents(now_ts);
                seeded = true;
            } else {
                ok = true;
            }
            store.SetMeta("last_sync_status", "mock");
            store.SetMeta("last_sync_error", "");
        } else {
            ok = SyncOnce(&store, &error);
            store.SetMeta("last_sync_status", ok ? "online" : "offline");
        }

        store.SetMeta("last_sync_ts", std::to_string(now_ts));
        if (!ok) {
            if (error.empty()) {
                error = "sync failed";
            }
            store.SetMeta("last_sync_error", error);
        } else {
            store.SetMeta("last_sync_error", "");
        }

        for (int i = 0; i < config_.sync_interval_sec && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    curl_global_cleanup();
}

bool CalendarSyncService::SyncOnce(EventStore* store, std::string* error) {
    if (!store) {
        if (error) {
            *error = "store null";
        }
        return false;
    }

    std::string ics_url = Trim(config_.ics_url);
    if (!ics_url.empty()) {
        if (!LooksLikeUrl(ics_url)) {
            std::cerr << "ICS URL is not a valid http(s) URL.\n";
            if (error) {
                *error = "ics_url invalid";
            }
            return false;
        }
        SyncConfig ics_config = config_;
        ics_config.ics_url = ics_url;
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "libcurl init failed\n";
            if (error) {
                *error = "curl init failed";
            }
            return false;
        }
        bool ok = FetchIcsEvents(curl, ics_config, store, TimeUtil::NowTs(), error);
        curl_easy_cleanup(curl);
        return ok;
    }

    if (error) {
        *error = "ics_url empty";
    }
    return false;
}
