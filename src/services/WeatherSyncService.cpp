#include "services/WeatherSyncService.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

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

bool HttpGet(CURL* curl, const std::string& url, HttpResponse* out) {
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

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Weather HTTP GET failed: " << curl_easy_strerror(res) << "\n";
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->code);
    return true;
}

bool IsValidCoords(double latitude, double longitude) {
    return std::isfinite(latitude) &&
           std::isfinite(longitude) &&
           latitude >= -90.0 &&
           latitude <= 90.0 &&
           longitude >= -180.0 &&
           longitude <= 180.0;
}

std::string FormatDecimal1(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

std::string WeatherCodeText(int code, bool is_day) {
    switch (code) {
        case 0: return is_day ? "Clear sky" : "Clear night";
        case 1: return is_day ? "Mostly clear" : "Mostly clear night";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45:
        case 48:
            return "Fog";
        case 51:
        case 53:
        case 55:
            return "Drizzle";
        case 56:
        case 57:
            return "Freezing drizzle";
        case 61:
        case 63:
        case 65:
            return "Rain";
        case 66:
        case 67:
            return "Freezing rain";
        case 71:
        case 73:
        case 75:
            return "Snow";
        case 77:
            return "Snow grains";
        case 80:
        case 81:
        case 82:
            return "Rain showers";
        case 85:
        case 86:
            return "Snow showers";
        case 95:
            return "Thunderstorm";
        case 96:
        case 99:
            return "Thunder + hail";
        default:
            return "Weather";
    }
}

std::string BuildOpenMeteoUrl(const WeatherConfig& config) {
    std::ostringstream out;
    out << "https://api.open-meteo.com/v1/forecast"
        << "?latitude=" << std::fixed << std::setprecision(5) << config.latitude
        << "&longitude=" << std::fixed << std::setprecision(5) << config.longitude
        << "&current=temperature_2m,weather_code,is_day,wind_speed_10m"
        << "&timezone=auto";
    return out.str();
}

} // namespace

WeatherSyncService::WeatherSyncService(const WeatherConfig& config) : config_(config) {}

WeatherSyncService::~WeatherSyncService() {
    Stop();
}

void WeatherSyncService::Start() {
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread(&WeatherSyncService::Run, this);
}

void WeatherSyncService::Stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool WeatherSyncService::IsRunning() const {
    return running_.load();
}

void WeatherSyncService::Run() {
    EventStore store(config_.db_path);
    if (!store.Open()) {
        std::cerr << "WeatherSyncService: failed to open DB\n";
        return;
    }

    int interval = std::max(60, config_.sync_interval_sec);
    while (running_) {
        int64_t now_ts = TimeUtil::NowTs();
        bool ok = false;
        std::string error;
        std::string status = "offline";

        if (!config_.enabled) {
            ok = true;
            status = "disabled";
        } else if (!IsValidCoords(config_.latitude, config_.longitude)) {
            ok = true;
            status = "config";
            error = "weather lat/lon invalid";
        } else {
            ok = SyncOnce(&store, &error);
            status = ok ? "online" : "offline";
        }

        store.SetMeta("weather_status", status);
        if (status == "online") {
            store.SetMeta("weather_last_sync_ts", std::to_string(now_ts));
        }

        if (!ok) {
            if (error.empty()) {
                error = "weather sync failed";
            }
            store.SetMeta("weather_error", error);
        } else {
            store.SetMeta("weather_error", "");
        }

        for (int i = 0; i < interval && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

}

bool WeatherSyncService::SyncOnce(EventStore* store, std::string* error) {
    if (!store) {
        if (error) {
            *error = "store null";
        }
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) {
            *error = "curl init failed";
        }
        return false;
    }

    HttpResponse resp;
    std::string url = BuildOpenMeteoUrl(config_);
    bool request_ok = HttpGet(curl, url, &resp);
    curl_easy_cleanup(curl);
    if (!request_ok) {
        if (error) {
            *error = "weather http failed";
        }
        return false;
    }
    if (resp.code != 200) {
        if (error) {
            *error = "weather http " + std::to_string(resp.code);
        }
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("current") || !j["current"].is_object()) {
        if (error) {
            *error = "weather invalid json";
        }
        return false;
    }

    const auto& current = j["current"];
    if (!current.contains("temperature_2m") || !current.contains("weather_code")) {
        if (error) {
            *error = "weather missing fields";
        }
        return false;
    }

    double temperature = current.value("temperature_2m", std::nan(""));
    int weather_code = current.value("weather_code", -1);
    bool is_day = current.value("is_day", 1) == 1;
    double wind_kmh = current.value("wind_speed_10m", std::nan(""));

    if (!std::isfinite(temperature)) {
        if (error) {
            *error = "weather temperature invalid";
        }
        return false;
    }

    store->SetMeta("weather_temp_c", FormatDecimal1(temperature));
    store->SetMeta("weather_code", std::to_string(weather_code));
    store->SetMeta("weather_summary", WeatherCodeText(weather_code, is_day));
    if (std::isfinite(wind_kmh)) {
        store->SetMeta("weather_wind_kmh", FormatDecimal1(wind_kmh));
    }
    return true;
}
