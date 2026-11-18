#pragma once

#include "radio_player.h"
#include <cstdlib>
#include <cstring>
#include <utility>

// Only store the used text. A nationwide list of fixed RadioStation records
// would compete with the AAC decoder for tens of KiB of internal heap.
class RadioStationList {
public:
    RadioStationList() = default;
    RadioStationList(const RadioStationList &) = delete;
    RadioStationList &operator=(const RadioStationList &) = delete;
    ~RadioStationList() { clear(); }

    void clear() {
        if (!borrowed_) std::free(data_);
        data_ = nullptr;
        bytes_ = capacity_ = count_ = 0;
        borrowed_ = false;
    }
    std::size_t size() const { return count_; }

    bool append(const RadioStation &row) {
        const auto name = std::strlen(row.name) + 1;
        const auto desc = std::strlen(row.description) + 1;
        const auto url = std::strlen(row.url) + 1;
        const auto end = bytes_ + header + name + desc + url;
        if (end > capacity_) {
            const auto capacity = (end + 511) & ~std::size_t(511);
            if (borrowed_) return false;
            auto *data = static_cast<unsigned char *>(std::realloc(data_, capacity));
            if (!data) return false;
            data_ = data;
            capacity_ = capacity;
        }
        auto *p = data_ + bytes_;
        p[0] = name - 1;
        p[1] = desc - 1;
        p[2] = url - 1;
        std::memcpy(p + 3, &row.frequency_decihz, 2);
        std::memcpy(p + header, row.name, name);
        std::memcpy(p + header + name, row.description, desc);
        std::memcpy(p + header + name + desc, row.url, url);
        bytes_ = end;
        ++count_;
        return true;
    }

    RadioStation at(std::size_t index) const {
        RadioStation row{};
        if (index >= count_) return row;
        const auto *p = data_;
        while (index--) p += length(p);
        std::memcpy(&row.frequency_decihz, p + 3, 2);
        std::memcpy(row.name, p + header, p[0] + 1);
        std::memcpy(row.description, p + header + p[0] + 1, p[1] + 1);
        std::memcpy(row.url, p + header + p[0] + p[1] + 2, p[2] + 1);
        return row;
    }

    bool contains_url(const char *url) const {
        for (std::size_t pos = 0; pos < bytes_; pos += length(data_ + pos)) {
            const auto *p = data_ + pos;
            if (!std::strcmp(reinterpret_cast<const char *>(p + header + p[0] + p[1] + 2), url))
                return true;
        }
        return false;
    }

    // Transfer the finished list under the player lock. Prefer the existing
    // audio workspace; larger catalogs keep their discovery allocation.
    void take(RadioStationList &other, unsigned char *workspace, std::size_t available) {
        clear();
        bytes_ = other.bytes_;
        count_ = other.count_;
        if (bytes_ && bytes_ <= available) {
            data_ = workspace;
            borrowed_ = true;
            capacity_ = available;
            std::memcpy(data_, other.data_, bytes_);
        } else {
            data_ = std::exchange(other.data_, nullptr);
            capacity_ = other.capacity_;
            borrowed_ = other.borrowed_;
        }
        other.clear();
    }

private:
    // Each record starts with three one-byte string lengths and a 16-bit
    // frequency, followed by the three NUL-terminated strings.
    static_assert(sizeof(RadioStation::name) <= 256 && sizeof(RadioStation::description) <= 256 &&
                  sizeof(RadioStation::url) <= 256);
    static constexpr std::size_t header = 5;
    static std::size_t length(const unsigned char *p) { return header + p[0] + p[1] + p[2] + 3; }
    unsigned char *data_ = nullptr;
    std::size_t bytes_ = 0, capacity_ = 0, count_ = 0;
    bool borrowed_ = false;
};
