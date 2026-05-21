#pragma once

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orchestration::sqlite_helpers {

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline void execOrThrow(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return;
    }
    const std::string message = err ? err : "sqlite error";
    if (err) {
        sqlite3_free(err);
    }
    throw std::runtime_error(message);
}

inline bool execSql(sqlite3* db, const char* sql, std::string& error) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }
    error = err ? std::string(err) : "sqlite exec failed";
    if (err) {
        sqlite3_free(err);
    }
    return false;
}

inline void bindText(sqlite3_stmt* stmt, int index, std::string_view value) {
    sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

inline std::string columnText(sqlite3_stmt* stmt, int index) {
    const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    return raw ? std::string(raw) : std::string{};
}

inline std::string sqliteError(sqlite3* db, std::string_view prefix) {
    return std::string(prefix) + ": " + (db ? sqlite3_errmsg(db) : "sqlite error");
}

} // namespace orchestration::sqlite_helpers
