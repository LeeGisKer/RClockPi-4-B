#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct sqlite3;

struct EventRecord {
    std::string id;
    std::string calendar_id;
    std::string title;
    int64_t start_ts = 0;
    int64_t end_ts = 0;
    bool all_day = false;
    std::string location;
    int64_t updated_ts = 0;
    std::string status;
};

class EventStore {
public:
    explicit EventStore(const std::string& db_path);
    ~EventStore();

    bool Open();
    void Close();
    bool InitSchema();

    bool UpsertEvent(const EventRecord& ev);
    bool GetNextEventAfter(int64_t ts, EventRecord* out);
    std::vector<EventRecord> GetEventsForDay(int64_t day_ts);
    std::map<int, int> GetEventDaysInMonth(int year, int month);

    bool SetMeta(const std::string& key, const std::string& value);
    std::string GetMeta(const std::string& key);

    bool InsertSampleEvents(int64_t now_ts);

private:
    bool Exec(const std::string& sql);

    std::string db_path_;
    sqlite3* db_ = nullptr;
};
