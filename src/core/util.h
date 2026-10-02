// Small file and string helpers shared by the core.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace cl {

namespace fs = std::filesystem;

bool read_file(const fs::path& path, std::string& out);
// Write to a temporary file next to path, then rename over it.
bool write_file_atomic(const fs::path& path, const std::string& data);

std::string trim(const std::string& s);
// First whitespace-separated token ("<sha256>  <name>" -> "<sha256>").
std::string first_token(const std::string& s);
std::string human_size(uint64_t bytes);
std::string now_iso8601();

// Ids are directory names under images/: [A-Za-z0-9._+-]{1,64}, no leading dot.
bool valid_id(const std::string& id);
// Ids the index may not use (local state the launcher manages itself).
bool reserved_id(const std::string& id);

}  // namespace cl
