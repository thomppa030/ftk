#pragma once

#include "ftk/base/result.hpp"
#include "renderer/resources/fjinst.hpp"

#include <string>

namespace fjell {

/// Read a .fjinst from disk. Fails on a bad magic, an unsupported version,
/// or a truncated file.
[[nodiscard]] Result<FjInstData> load_fjinst(const std::string& path);

/// Write a .fjinst. `data.header` is rewritten from the record count.
[[nodiscard]] Result<> save_fjinst(const std::string& path, const FjInstData& data);

} // namespace fjell
