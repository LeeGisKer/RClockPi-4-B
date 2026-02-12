#pragma once

#include <atomic>
#include <string>
#include <thread>

class EventStore;

struct WeatherConfig {
    std::string db_path;
    bool enabled = false;
    double latitude = 0.0;
    double longitude = 0.0;
    int sync_interval_sec = 900;
};

class WeatherSyncService {
public:
    explicit WeatherSyncService(const WeatherConfig& config);
    ~WeatherSyncService();

    void Start();
    void Stop();
    bool IsRunning() const;

private:
    void Run();
    bool SyncOnce(EventStore* store, std::string* error);

    WeatherConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
