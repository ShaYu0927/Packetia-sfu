#pragma once
#include "RecordingRecords.h"
#include <mutex>
struct sqlite3;

namespace service {
// One service owns this catalog. Calls are serialized and transactions are
// synchronous: a successful StopRecording includes completed index writes.
class RecordingCatalog final {
public:
    explicit RecordingCatalog(const std::string& database_path);
    ~RecordingCatalog();
    RecordingCatalog(const RecordingCatalog&) = delete;
    RecordingCatalog& operator=(const RecordingCatalog&) = delete;
    void Store(const SegmentInfo& segment);
    std::vector<SegmentInfo> Query(const SegmentQuery& query) const;
    std::vector<RecordedStream> Streams(const std::string& session_id) const;
private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};
}
