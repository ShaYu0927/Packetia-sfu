#include "RecordingCatalog.h"
#include "third/nlohmann/json.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <filesystem>

namespace service {
namespace {
using Json = nlohmann::json;
void Exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("recording index: ") + sqlite3_errmsg(db));
}
struct Statement {
    sqlite3* db;
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* d, const char* sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void Bind(int index, const std::string& text) {
        if (sqlite3_bind_text(value, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    void Bind(int index, int64_t number) {
        if (sqlite3_bind_int64(value, index, number) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    bool Row() {
        const int result = sqlite3_step(value);
        if (result != SQLITE_ROW && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return result == SQLITE_ROW;
    }
    std::string Text(int column) const {
        const auto* text = sqlite3_column_text(value, column);
        return text ? reinterpret_cast<const char*>(text) : "";
    }
};
Json Encode(const SegmentInfo& s) {
    // OS error messages can use the process locale rather than UTF-8 (notably
    // filesystem_error on Windows). Preserve IDs strictly, but replace invalid
    // bytes in diagnostic text so failure reporting itself can be persisted.
    const auto error = Json::parse(Json(s.error).dump(-1, ' ', false, Json::error_handler_t::replace));
    Json tracks = Json::array();
    for (const auto& t : s.tracks) tracks.push_back({{"endpoint_id", t.endpoint_id}, {"track_id", t.track_id},
        {"codec", t.codec}, {"width", t.width}, {"height", t.height}, {"sample_rate", t.sample_rate}, {"channels", t.channels}});
    return {{"recording_id", s.recording_id}, {"segment_id", s.segment_id}, {"session_id", s.session_id},
        {"stream_id", s.stream_id}, {"sequence", s.sequence}, {"media_start_us", s.media_start_us},
        {"media_end_us", s.media_end_us}, {"started_at_ms", s.started_at_ms}, {"ended_at_ms", s.ended_at_ms},
        {"relative_path", s.relative_path}, {"size_bytes", s.size_bytes}, {"status", static_cast<int>(s.status)},
        {"tracks", tracks}, {"error", error}};
}
SegmentInfo Decode(const std::string& text) {
    const auto j = Json::parse(text);
    SegmentInfo s;
    s.recording_id = j.at("recording_id"); s.segment_id = j.at("segment_id");
    s.session_id = j.at("session_id"); s.stream_id = j.at("stream_id"); s.sequence = j.at("sequence");
    s.media_start_us = j.at("media_start_us"); s.media_end_us = j.at("media_end_us");
    s.started_at_ms = j.at("started_at_ms"); s.ended_at_ms = j.at("ended_at_ms");
    s.relative_path = j.at("relative_path"); s.size_bytes = j.at("size_bytes");
    s.status = static_cast<SegmentStatus>(j.at("status").get<int>()); s.error = j.at("error");
    for (const auto& t : j.at("tracks")) s.tracks.push_back({t.at("endpoint_id"), t.at("track_id"),
        t.at("codec"), t.at("width"), t.at("height"), t.at("sample_rate"), t.at("channels")});
    return s;
}
}

RecordingCatalog::RecordingCatalog(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const int result = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    try {
        if (result != SQLITE_OK) throw std::runtime_error("cannot open recording index: " + path);
        sqlite3_busy_timeout(db_, 5000);
        Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        Statement version(db_, "PRAGMA user_version");
        version.Row();
        const int schema = sqlite3_column_int(version.value, 0);
        if (schema != 0 && schema != 1) throw std::runtime_error("unsupported recording index schema");
        Exec(db_, "BEGIN; CREATE TABLE IF NOT EXISTS segments ("
            "segment_id TEXT PRIMARY KEY, recording_id TEXT NOT NULL, session_id TEXT NOT NULL,"
            "stream_id TEXT NOT NULL, sequence INTEGER NOT NULL, started_at_ms INTEGER NOT NULL,"
            "ended_at_ms INTEGER NOT NULL, status INTEGER NOT NULL, payload TEXT NOT NULL,"
            "UNIQUE(recording_id, sequence));"
            "CREATE INDEX IF NOT EXISTS segment_session_time ON segments(session_id, started_at_ms);"
            "CREATE INDEX IF NOT EXISTS segment_stream_time ON segments(session_id, stream_id, started_at_ms);"
            "PRAGMA user_version=1; COMMIT;");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
RecordingCatalog::~RecordingCatalog() { if (db_) sqlite3_close(db_); }

void RecordingCatalog::Store(const SegmentInfo& s) {
    if (s.segment_id.empty() || s.recording_id.empty() || s.session_id.empty() || s.stream_id.empty())
        throw std::invalid_argument("recording index requires segment, recording, session and stream IDs");
    const auto payload = Encode(s).dump();
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "INSERT INTO segments VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(segment_id) DO UPDATE SET ended_at_ms=excluded.ended_at_ms,"
        "status=excluded.status,payload=excluded.payload WHERE segments.status=0");
    statement.Bind(1, s.segment_id); statement.Bind(2, s.recording_id);
    statement.Bind(3, s.session_id); statement.Bind(4, s.stream_id);
    statement.Bind(5, static_cast<int64_t>(s.sequence)); statement.Bind(6, s.started_at_ms);
    statement.Bind(7, s.ended_at_ms); statement.Bind(8, static_cast<int64_t>(s.status));
    statement.Bind(9, payload); statement.Row();
}

std::vector<SegmentInfo> RecordingCatalog::Query(const SegmentQuery& q) const {
    if (!q.limit || q.limit > 1000 || q.from_ms < 0 || q.to_ms <= q.from_ms)
        throw std::invalid_argument("invalid segment query range or limit (1..1000)");
    std::lock_guard<std::mutex> lock(mutex_);
    // Construct predicates only from fixed SQL; identifiers and ranges are bound.
    std::string sql = "SELECT payload FROM segments WHERE started_at_ms < ? AND (ended_at_ms > ? OR (status=0 AND ended_at_ms=0))";
    if (!q.session_id.empty()) sql += " AND session_id=?";
    if (!q.stream_id.empty()) sql += " AND stream_id=?";
    if (!q.recording_id.empty()) sql += " AND recording_id=?";
    if (q.completed_only) sql += " AND status=1";
    sql += " ORDER BY started_at_ms,recording_id,sequence LIMIT ? OFFSET ?";
    Statement statement(db_, sql.c_str());
    int i = 1;
    statement.Bind(i++, q.to_ms); statement.Bind(i++, q.from_ms);
    if (!q.session_id.empty()) statement.Bind(i++, q.session_id);
    if (!q.stream_id.empty()) statement.Bind(i++, q.stream_id);
    if (!q.recording_id.empty()) statement.Bind(i++, q.recording_id);
    statement.Bind(i++, static_cast<int64_t>(q.limit)); statement.Bind(i++, static_cast<int64_t>(q.offset));
    std::vector<SegmentInfo> result;
    while (statement.Row()) result.push_back(Decode(statement.Text(0)));
    return result;
}

std::vector<RecordedStream> RecordingCatalog::Streams(const std::string& session) const {
    if (session.empty()) throw std::invalid_argument("session_id is required");
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "SELECT DISTINCT recording_id,session_id,stream_id FROM segments WHERE session_id=? ORDER BY stream_id,recording_id");
    statement.Bind(1, session);
    std::vector<RecordedStream> result;
    while (statement.Row()) result.push_back({statement.Text(0), statement.Text(1), statement.Text(2)});
    return result;
}
} // namespace service
