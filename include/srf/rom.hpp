#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace srf {
inline constexpr std::size_t rom_size = 1U << 20;
inline constexpr char supported_sha256[] =
    "c04d80b84514202ff319384ca20641eb0189e975eed5612915bd9c224b2ab30a";

std::string sha256(std::span<const std::uint8_t> data);
std::string json_string(const std::string& value);

// Only verified USA Rev 1 data enters the native tools. A copier header is
// ignored in memory; the source file is never opened for writing.
class Rom {
public:
    static Rom load(const std::filesystem::path& path);
    static Rom from_bytes(std::vector<std::uint8_t> bytes);
    std::span<const std::uint8_t> bytes() const { return bytes_; }
    std::span<const std::uint8_t> slice(std::size_t offset, std::size_t size) const;
    std::uint16_t u16(std::size_t offset) const;
    bool had_copier_header() const { return headered_; }
    std::string manifest() const;
    static std::size_t lorom_offset(std::uint32_t address);

private:
    std::vector<std::uint8_t> bytes_;
    bool headered_ = false;
};
}
