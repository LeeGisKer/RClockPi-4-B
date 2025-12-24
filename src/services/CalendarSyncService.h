#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

class EventStore;

struct SyncConfig {
    std::string db_path;
    std::string token_path;
    std::string ics_url;
    std::vector<std::string> calendar_ids;
    int sync_interval_sec = 120;
    int time_window_days = 14;
    bool mock_mode = false;
    std::string client_id;
    std::string client_secret;
};

class CalendarSyncService {
public:
    explicit CalendarSyncService(const SyncConfig& config);
    ~CalendarSyncService();

    void Start();
    void Stop();
    bool IsRunning() const;

private:
    void Run();
    bool SyncOnce(EventStore* store, std::string* error);

    SyncConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
