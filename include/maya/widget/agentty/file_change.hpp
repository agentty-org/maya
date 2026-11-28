#pragma once
// maya::FileChange / FileChangeKind — one changed file, as data.
//
// Split out of file_changes.hpp so a widget that only needs the record (the
// changes strip) does not pull in the FileChanges list widget with it.

#include <cstdint>
#include <string>

namespace maya {

enum class FileChangeKind : uint8_t {
    Created,
    Modified,
    Deleted,
    Renamed,
};

struct FileChange {
    std::string    path;
    FileChangeKind kind        = FileChangeKind::Modified;
    int            lines_added = 0;
    int            lines_removed = 0;
};

} // namespace maya
