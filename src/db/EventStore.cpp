#include "db/EventStore.h"

#include "util/TimeUtil.h"

#include <sqlite3.h>
#include <iostream>
#include <memory>

namespace {

struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

StmtPtr Prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "SQLite prepare failed: " << sqlite3_errmsg(db) << "\n";
        return nullptr;
    }
    return StmtPtr(stmt);
}

std::string ColumnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

} // namespace

EventStore::EventStore(const std::string& db_path) : db_path_(db_path) {}

EventStore::~EventStore() {
    Close();
}

bool EventStore::Open() {
    if (sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::cerr << "SQLite open failed: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    Exec("PRAGMA journal_mode=WAL;");
    Exec("PRAGMA synchronous=NORMAL;");
    return InitSchema();
}

void EventStore::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool EventStore::Exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "SQLite exec failed: " << (err ? err : "unknown") << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool EventStore::InitSchema() {
    const char* events_sql =
        "CREATE TABLE IF NOT EXISTS events("
        "id TEXT PRIMARY KEY,"
        "calendar_id TEXT,"
        "title TEXT,"
        "start_ts INTEGER,"
        "end_ts INTEGER,"
        "all_day INTEGER,"
        "location TEXT,"
        "updated_ts INTEGER,"
        "status TEXT"
        ");";

    const char* meta_sql =
        "CREATE TABLE IF NOT EXISTS meta("
        "key TEXT PRIMARY KEY,"
        "value TEXT"
        ");";

    const char* index_sql =
        "CREATE INDEX IF NOT EXISTS idx_events_start ON events(start_ts);";

    return Exec(events_sql) && Exec(meta_sql) && Exec(index_sql);
}

bool EventStore::UpsertEvent(const EventRecord& ev) {
    const char* sql =
        "INSERT INTO events(id, calendar_id, title, start_ts, end_ts, all_day, location, updated_ts, status)"
        " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(id) DO UPDATE SET"
        " calendar_id=excluded.calendar_id,"
        " title=excluded.title,"
        " start_ts=excluded.start_ts,"
        " end_ts=excluded.end_ts,"
        " all_day=excluded.all_day,"
        " location=excluded.location,"
        " updated_ts=excluded.updated_ts,"
        " status=excluded.status";

    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, ev.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, ev.calendar_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, ev.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, ev.start_ts);
    sqlite3_bind_int64(stmt.get(), 5, ev.end_ts);
    sqlite3_bind_int(stmt.get(), 6, ev.all_day ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 7, ev.location.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 8, ev.updated_ts);
    sqlite3_bind_text(stmt.get(), 9, ev.status.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        std::cerr << "SQLite upsert failed: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }
    return true;
}

bool EventStore::GetNextEventAfter(int64_t ts, EventRecord* out) {
    const char* sql =
        "SELECT id, calendar_id, title, start_ts, end_ts, all_day, location, updated_ts, status"
        " FROM events WHERE start_ts >= ? AND status != 'cancelled'"
        " ORDER BY start_ts ASC LIMIT 1";

    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return false;
    }

    sqlite3_bind_int64(stmt.get(), 1, ts);

    int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        out->id = ColumnText(stmt.get(), 0);
        out->calendar_id = ColumnText(stmt.get(), 1);
        out->title = ColumnText(stmt.get(), 2);
        out->start_ts = sqlite3_column_int64(stmt.get(), 3);
        out->end_ts = sqlite3_column_int64(stmt.get(), 4);
        out->all_day = sqlite3_column_int(stmt.get(), 5) != 0;
        out->location = ColumnText(stmt.get(), 6);
        out->updated_ts = sqlite3_column_int64(stmt.get(), 7);
        out->status = ColumnText(stmt.get(), 8);
        return true;
    }

    return false;
}

std::vector<EventRecord> EventStore::GetEventsForDay(int64_t day_ts) {
    std::vector<EventRecord> out;
    int64_t start = TimeUtil::StartOfDay(day_ts);
    int64_t end = TimeUtil::EndOfDay(day_ts);

    const char* sql =
        "SELECT id, calendar_id, title, start_ts, end_ts, all_day, location, updated_ts, status"
        " FROM events WHERE start_ts <= ? AND end_ts >= ? AND status != 'cancelled'"
        " ORDER BY start_ts ASC";

    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return out;
    }

    sqlite3_bind_int64(stmt.get(), 1, end);
    sqlite3_bind_int64(stmt.get(), 2, start);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        EventRecord ev;
        ev.id = ColumnText(stmt.get(), 0);
        ev.calendar_id = ColumnText(stmt.get(), 1);
        ev.title = ColumnText(stmt.get(), 2);
        ev.start_ts = sqlite3_column_int64(stmt.get(), 3);
        ev.end_ts = sqlite3_column_int64(stmt.get(), 4);
        ev.all_day = sqlite3_column_int(stmt.get(), 5) != 0;
        ev.location = ColumnText(stmt.get(), 6);
        ev.updated_ts = sqlite3_column_int64(stmt.get(), 7);
        ev.status = ColumnText(stmt.get(), 8);
        out.push_back(std::move(ev));
    }
    return out;
}

std::map<int, int> EventStore::GetEventDaysInMonth(int year, int month) {
    std::map<int, int> counts;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;

    int64_t start_ts = std::mktime(&tm);
    tm.tm_mday = TimeUtil::DaysInMonth(year, month);
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    int64_t end_ts = std::mktime(&tm);

    const char* sql =
        "SELECT start_ts FROM events WHERE start_ts >= ? AND start_ts <= ? AND status != 'cancelled'";

    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return counts;
    }

    sqlite3_bind_int64(stmt.get(), 1, start_ts);
    sqlite3_bind_int64(stmt.get(), 2, end_ts);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        int64_t ts = sqlite3_column_int64(stmt.get(), 0);
        std::tm local = TimeUtil::LocalTime(ts);
        counts[local.tm_mday] += 1;
    }
    return counts;
}

bool EventStore::SetMeta(const std::string& key, const std::string& value) {
    const char* sql =
        "INSERT INTO meta(key, value) VALUES(?, ?)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value";

    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        std::cerr << "SQLite set meta failed: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }
    return true;
}

std::string EventStore::GetMeta(const std::string& key) {
    const char* sql = "SELECT value FROM meta WHERE key = ?";
    auto stmt = Prepare(db_, sql);
    if (!stmt) {
        return "";
    }

    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt.get(), 0);
        if (text) {
            return reinterpret_cast<const char*>(text);
        }
    }
    return "";
}

bool EventStore::InsertSampleEvents(int64_t now_ts) {
    std::tm tm_today = TimeUtil::LocalTime(now_ts);
    tm_today.tm_hour = 9;
    tm_today.tm_min = 0;
    tm_today.tm_sec = 0;

    auto make_event = [&](const std::string& id, const std::string& title, int day_offset, int hour, int minutes, int duration_min, bool all_day) {
        std::tm tm = tm_today;
        tm.tm_mday += day_offset;
        tm.tm_hour = hour;
        tm.tm_min = minutes;
        tm.tm_sec = 0;
        int64_t start = std::mktime(&tm);
        int64_t end = start + duration_min * 60;

        EventRecord ev;
        ev.id = id;
        ev.calendar_id = "mock";
        ev.title = title;
        ev.start_ts = start;
        ev.end_ts = end;
        ev.all_day = all_day;
        ev.location = "";
        ev.updated_ts = now_ts;
        ev.status = "confirmed";
        return ev;
    };

    EventRecord e1 = make_event("mock-1", "Breakfast with Sam", 0, 8, 30, 60, false);
    EventRecord e2 = make_event("mock-2", "Design review", 0, 11, 0, 45, false);
    EventRecord e3 = make_event("mock-3", "Gym", 0, 18, 0, 90, false);
    EventRecord e4 = make_event("mock-4", "Project kickoff", 1, 10, 0, 60, false);
    EventRecord e5 = make_event("mock-5", "All-day focus", 2, 0, 0, 24 * 60, true);
    EventRecord e6 = make_event("mock-6", "Dinner", 3, 19, 0, 90, false);

    bool ok = true;
    ok &= UpsertEvent(e1);
    ok &= UpsertEvent(e2);
    ok &= UpsertEvent(e3);
    ok &= UpsertEvent(e4);
    ok &= UpsertEvent(e5);
    ok &= UpsertEvent(e6);
    return ok;
}
