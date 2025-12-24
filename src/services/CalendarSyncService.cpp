#include "services/CalendarSyncService.h"

#include "db/EventStore.h"
#include "util/TimeUtil.h"

#include <curl/curl.h>
#include <chrono>
#include <iostream>
#include <thread>

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
    EventStore store(config_.db_path);
    if (!store.Open()) {
        std::cerr << "CalendarSyncService: failed to open DB\n";
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
            ok = SyncOnce();
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
}

bool CalendarSyncService::SyncOnce() {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "libcurl init failed\n";
        return false;
    }

    // TODO: Implement OAuth refresh + Google Calendar events sync.
    curl_easy_cleanup(curl);
    return false;
}
