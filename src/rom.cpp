#include "srf/rom.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace srf {
std::string sha256(std::span<const std::uint8_t> data) {
    if (data.size() > ULONG_MAX) throw std::runtime_error("Hash input too large");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Windows SHA-256 provider unavailable");
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    std::array<unsigned char, 32> digest{};
    if (status >= 0) status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                                           static_cast<ULONG>(data.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(),
                                             static_cast<ULONG>(digest.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Windows SHA-256 operation failed");
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest) stream << std::setw(2) << unsigned(byte);
    return stream.str();
}

std::string json_string(const std::string& value) {
    std::ostringstream stream;
    stream << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (c < 32) stream << "\\u" << std::hex << std::setw(4)
                               << std::setfill('0') << unsigned(c) << std::dec;
            else stream << c;
        }
    }
    stream << '"';
    return stream.str();
}

Rom Rom::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open ROM for reading");
    const auto size = input.tellg();
    if (size != static_cast<std::streamoff>(rom_size) &&
        size != static_cast<std::streamoff>(rom_size + 512))
        throw std::runtime_error("Expected a 1 MiB ROM, optionally with a 512-byte copier header");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Incomplete ROM read");
    return from_bytes(std::move(bytes));
}

Rom Rom::from_bytes(std::vector<std::uint8_t> bytes) {
    Rom result;
    if (bytes.size() == rom_size + 512) {
        result.headered_ = true;
        bytes.erase(bytes.begin(), bytes.begin() + 512);
    }
    if (bytes.size() != rom_size)
        throw std::runtime_error("Unsupported ROM size");
    const auto digest = sha256(bytes);
    if (digest != supported_sha256)
        throw std::runtime_error("Unsupported or modified ROM. Expected USA Rev 1 SHA-256 " +
            std::string(supported_sha256) + "; received " + digest);
    result.bytes_ = std::move(bytes);
    const auto sum = std::accumulate(result.bytes_.begin(), result.bytes_.end(), 0U) & 0xffffU;
    if (sum != result.u16(0x7fde) ||
        (result.u16(0x7fdc) ^ result.u16(0x7fde)) != 0xffff)
        throw std::runtime_error("ROM internal checksum mismatch");
    return result;
}

std::span<const std::uint8_t> Rom::slice(std::size_t offset, std::size_t size) const {
    if (offset > bytes_.size() || size > bytes_.size() - offset)
        throw std::out_of_range("ROM range out of bounds");
    return std::span(bytes_).subspan(offset, size);
}

std::uint16_t Rom::u16(std::size_t offset) const {
    const auto value = slice(offset, 2);
    return static_cast<std::uint16_t>(value[0] | (unsigned(value[1]) << 8));
}

std::size_t Rom::lorom_offset(std::uint32_t address) {
    // Deliberately restrict this helper to the verified 32 upper-half ROM
    // banks and their CPU mirrors. It is not a full cartridge bus emulator.
    const auto bank = (address >> 16) & 0x7fU;
    if (address > 0xffffff || bank >= 0x20 || (address & 0xffffU) < 0x8000)
        throw std::out_of_range("Address is outside the supported LoROM code window");
    return bank * 0x8000U + (address & 0x7fffU);
}

std::string Rom::manifest() const {
    std::ostringstream stream;
    stream << "{\n  \"schema_version\": 1,\n"
           << "  \"game\": \"Stunt Race FX\",\n"
           << "  \"revision\": \"USA Rev 1\",\n"
           << "  \"sha256\": " << json_string(supported_sha256) << ",\n"
           << "  \"normalized_size\": " << bytes_.size() << ",\n"
           << "  \"copier_header_removed\": " << (headered_ ? "true" : "false") << ",\n"
           << "  \"map_mode\": " << unsigned(bytes_[0x7fd5]) << ",\n"
           << "  \"cartridge_type\": " << unsigned(bytes_[0x7fd6]) << ",\n"
           << "  \"checksum\": " << u16(0x7fde) << ",\n"
           << "  \"reset_vector\": " << u16(0x7ffc) << ",\n"
           << "  \"native_race_ready\": true,\n"
           << "  \"scope\": \"Native desktop Speed Trax (all 12 courses), Stunt Trax (Rock Field, Up'n Down and Ice Dance)\"\n}\n";
    return stream.str();
}
}
