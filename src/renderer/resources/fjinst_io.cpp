#include "renderer/resources/fjinst_io.hpp"

#include <cstdio>
#include <memory>

namespace fjell {

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// A corrupt count fails cleanly instead of asking for gigabytes.
constexpr uint32_t MAX_RECORDS = 16u * 1024u * 1024u;

} // namespace

Result<FjInstData> load_fjinst(const std::string& path) {
    FilePtr file{std::fopen(path.c_str(), "rb")};
    if (!file) {
        return make_error("fjinst: cannot open " + path);
    }

    FjInstData data;
    if (std::fread(&data.header, sizeof(data.header), 1, file.get()) != 1) {
        return make_error("fjinst: truncated header in " + path);
    }
    if (data.header.magic != FJINST_MAGIC) {
        return make_error("fjinst: bad magic in " + path);
    }
    if (data.header.version != FJINST_VERSION) {
        return make_error("fjinst: unsupported version " +
                          std::to_string(data.header.version) + " in " + path);
    }
    if (data.header.count > MAX_RECORDS) {
        return make_error("fjinst: record count " + std::to_string(data.header.count) +
                          " is not plausible in " + path);
    }

    data.records.resize(data.header.count);
    if (data.header.count > 0 &&
        std::fread(data.records.data(), sizeof(FjInstRecord), data.records.size(),
                   file.get()) != data.records.size()) {
        return make_error("fjinst: truncated records in " + path);
    }
    return data;
}

Result<> save_fjinst(const std::string& path, const FjInstData& data) {
    FilePtr file{std::fopen(path.c_str(), "wb")};
    if (!file) {
        return make_error("fjinst: cannot write " + path);
    }

    FjInstHeader header = data.header;
    header.magic = FJINST_MAGIC;
    header.version = FJINST_VERSION;
    header.count = static_cast<uint32_t>(data.records.size());

    if (std::fwrite(&header, sizeof(header), 1, file.get()) != 1) {
        return make_error("fjinst: failed writing header to " + path);
    }
    if (!data.records.empty() &&
        std::fwrite(data.records.data(), sizeof(FjInstRecord), data.records.size(),
                    file.get()) != data.records.size()) {
        return make_error("fjinst: failed writing records to " + path);
    }
    return {};
}

} // namespace fjell
