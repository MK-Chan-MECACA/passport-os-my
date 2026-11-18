#include "radio_catalog.h"
#include "radio_presets.h"
#include "radio_json_stream.h"
#include "esp_random.h"
#include <netdb.h>
#include <arpa/inet.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kTag[] = "radio_catalog";
constexpr char kLocationUrl[] =
    "http://ip-api.com/json/?fields=status,countryCode,regionName,city";
constexpr char kBackupLocationUrl[] =
    "https://ipwho.is/?fields=success,country_code,region,city";
constexpr std::size_t kObjectCapacity = 8192;
constexpr int kHttpTimeoutMs = 2500;
bool cancelled(RadioCatalogCancelled fn) { return fn && fn(); }


struct CityPlace {
    const char *name;
    const char *state;
};

// Source-generated GeoNames places (population >= 5000) with their state.
constexpr CityPlace kCities[] = {
#include "radio_cities.inc"
};
// GeoNames state/federal-territory names plus IP-provider and official spellings.
struct State { const char *alias; const char *name; };
constexpr State kStates[] = {
    {"Johor","Johor"},{"Kedah","Kedah"},{"Kelantan","Kelantan"},{"Melaka","Melaka"},
    {"Negeri Sembilan","Negeri Sembilan"},{"Pahang","Pahang"},{"Penang","Penang"},
    {"Perak","Perak"},{"Perlis","Perlis"},{"Sabah","Sabah"},{"Sarawak","Sarawak"},
    {"Selangor","Selangor"},{"Terengganu","Terengganu"},{"Kuala Lumpur","Kuala Lumpur"},
    {"Labuan","Labuan"},{"Putrajaya","Putrajaya"},
    {"Malacca","Melaka"},{"Pulau Pinang","Penang"},{"Negri Sembilan","Negeri Sembilan"},
    {"Federal Territory of Kuala Lumpur","Kuala Lumpur"},
    {"Wilayah Persekutuan Kuala Lumpur","Kuala Lumpur"},
    {"Federal Territory of Labuan","Labuan"},{"Wilayah Persekutuan Labuan","Labuan"},
    {"Federal Territory of Putrajaya","Putrajaya"},{"Wilayah Persekutuan Putrajaya","Putrajaya"},
};
bool same_name(const char *a, const char *b) {
    // Ignore ASCII punctuation, spaces and case.
    while (*a || *b) {
        while (*a && static_cast<unsigned char>(*a) < 128 && !std::isalnum(*a)) ++a;
        while (*b && static_cast<unsigned char>(*b) < 128 && !std::isalnum(*b)) ++b;
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
        if (!*a || !*b) return *a == *b;
        ++a; ++b;
    }
    return true;
}
const State *find_province(const char *name) {
    for (const auto &s : kStates) {
        const std::size_t length = std::strlen(s.alias);
        // Royal titles follow the state name: "Selangor Darul Ehsan".
        if (same_name(name, s.alias) ||
            (!std::strncmp(name, s.alias, length) && !std::strncmp(name + length, " Darul ", 7))) return &s;
    }
    return nullptr;
}
const CityPlace *find_city(const char *name, const char *region) {
    const State *s = find_province(region);
    for (const auto &c : kCities) {
        if ((!s || !std::strcmp(s->name, c.state)) && same_name(name, c.name)) return &c;
    }
    return nullptr;
}
bool starts_with(const char *text, const char *prefix) {
    return text && prefix && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool contains(const char *text, const char *needle) {
    return text && needle && std::strstr(text, needle) != nullptr;
}

bool http_get(const char *url, char *response, std::size_t capacity,
              std::size_t *response_size, RadioCatalogCancelled stop) {
    if (!url || !response || capacity < 2) return false;
    response[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "Passport-OS-MY-Radio/0.2.0-my.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    if (starts_with(url, "https://")) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool success = false;
    std::size_t used = 0;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "GET %s returned HTTP %d", url, status);
            break;
        }

        while (used + 1 < capacity && !cancelled(stop)) {
            const int received = esp_http_client_read(
                client, response + used, static_cast<int>(capacity - used - 1));
            if (received < 0) {
                used = 0;
                break;
            }
            if (received == 0) {
                success = used > 0 && esp_http_client_is_complete_data_received(client);
                break;
            }
            used += static_cast<std::size_t>(received);
        }
        if (used + 1 >= capacity) {
            ESP_LOGW(kTag, "Response from %s exceeded %u bytes", url,
                     static_cast<unsigned>(capacity));
            success = false;
        }
    } while (false);

    response[used] = '\0';
    if (response_size) *response_size = used;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

const char *json_string(cJSON *object, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

void copy_text(char *target, std::size_t capacity, const char *source) {
    if (!target || capacity == 0) return;
    if (!source) source = "";
    std::size_t n = std::min(capacity - 1, std::strlen(source));
    while (n && (static_cast<unsigned char>(source[n]) & 0xC0) == 0x80) --n;
    std::memcpy(target, source, n);
    target[n] = '\0';
}

bool locate(RadioLocation *location, RadioCatalogCancelled stop) {
    char *response = static_cast<char *>(std::malloc(2048));
    if (!response) return false;
    bool success = false;
    if (http_get(kLocationUrl, response, 2048, nullptr, stop)) {
        cJSON *root = cJSON_Parse(response);
        if (root) {
            const char *status = json_string(root, "status");
            const char *country = json_string(root, "countryCode");
            const char *region = json_string(root, "regionName");
            const char *city = json_string(root, "city");
            if (std::strcmp(status, "success") == 0 && country[0] &&
                (city[0] || region[0])) {
                copy_text(location->country_code, sizeof(location->country_code), country);
                copy_text(location->region, sizeof(location->region), region);
                copy_text(location->city, sizeof(location->city), city);
                success = true;
            }
            cJSON_Delete(root);
        }
    }
    if (!success && !cancelled(stop) && http_get(kBackupLocationUrl, response, 2048, nullptr, stop)) {
        cJSON *root = cJSON_Parse(response);
        if (root) {
            cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
            const char *country = json_string(root, "country_code");
            const char *region = json_string(root, "region");
            const char *city = json_string(root, "city");
            if (cJSON_IsTrue(ok) && country[0] && (city[0] || region[0])) {
                copy_text(location->country_code, sizeof(location->country_code), country);
                copy_text(location->region, sizeof(location->region), region);
                copy_text(location->city, sizeof(location->city), city);
                success = true;
            }
            cJSON_Delete(root);
        }
    }
    std::free(response);
    return success;
}

void url_encode(const char *source, char *target, std::size_t capacity) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t out = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(source);
         p && *p && out + 1 < capacity; ++p) {
        const unsigned char value = *p;
        if (std::isalnum(value) || value == '-' || value == '_' || value == '.' ||
            value == '~') {
            target[out++] = static_cast<char>(value);
        } else if (out + 3 < capacity) {
            target[out++] = '%';
            target[out++] = kHex[value >> 4];
            target[out++] = kHex[value & 0x0F];
        } else {
            break;
        }
    }
    target[out] = '\0';
}

bool duplicate_url(const RadioStation *stations, std::size_t count, const char *url) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(stations[i].url, url) == 0) return true;
    }
    return false;
}

// FM broadcasting occupies 87.0-108.0 MHz. Station names in the public
// directory spell the frequency in three ways, so they are tried in order of
// confidence: an explicit "FM" prefix, a bare decimal such as "94.7", and
// finally a compact form such as "971" that stations use as a brand. The last
// form is the least reliable, so it is only accepted when the digits are not
// glued to other numbers and still land inside the broadcast band.
constexpr uint16_t kBandLowDecihz = 870;
constexpr uint16_t kBandHighDecihz = 1080;

bool in_band(long decihz) {
    return decihz >= kBandLowDecihz && decihz <= kBandHighDecihz;
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

// Reads "94.7", "94,7" or "1017" style digits at `text` and returns tenths of a MHz.
// Digit runs without a decimal point are ambiguous: "971" is 97.1 MHz while
// "101" is 101.0 MHz. Both readings are tried and the one inside the broadcast
// band wins, checking the tenths reading first because it is far more common.
long read_decihz(const char *text, std::size_t *consumed) {
    std::size_t whole = 0;
    while (is_digit(text[whole])) ++whole;
    if (whole < 2 || whole > 4) return 0;

    long value = 0;
    for (std::size_t i = 0; i < whole; ++i) value = value * 10 + (text[i] - '0');

    if ((text[whole] == '.' || text[whole] == ',') && is_digit(text[whole + 1])) {
        if (whole > 3 || is_digit(text[whole + 2])) return 0;
        *consumed = whole + 2;
        return value * 10 + (text[whole + 1] - '0');
    }

    *consumed = whole;
    if (whole == 2) return value * 10;      // "94" is 94.0 MHz
    if (in_band(value)) return value;       // "971" is 97.1 MHz
    if (in_band(value * 10)) return value * 10;  // "101" is 101.0 MHz
    return value;
}

uint16_t parse_frequency(const char *name) {
    if (!name) return 0;

    for (const char *p = name; p[0] && p[1]; ++p) {
        if ((p[0] != 'F' && p[0] != 'f') || (p[1] != 'M' && p[1] != 'm')) continue;
        const char *digits = p + 2;
        while (*digits == ' ') ++digits;
        std::size_t consumed = 0;
        const long value = read_decihz(digits, &consumed);
        if (in_band(value)) return static_cast<uint16_t>(value);
    }

    for (const char *p = name; *p; ++p) {
        if (!is_digit(*p)) continue;
        const bool starts_run = p == name || (!is_digit(p[-1]) && p[-1] != '.' && p[-1] != ',');
        if (!starts_run) continue;
        std::size_t consumed = 0;
        const long value = read_decihz(p, &consumed);
        if (consumed && in_band(value)) return static_cast<uint16_t>(value);
        while (is_digit(p[1])) ++p;
    }
    return 0;
}

bool enabled(cJSON *entry, const char *name, bool default_value) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return cJSON_IsNumber(v) ? v->valueint != 0 : default_value;
}

bool parse_station(const char *json, const char *description, RadioStation &candidate) {
    cJSON *entry = cJSON_Parse(json);
    if (!entry || !cJSON_IsObject(entry)) { cJSON_Delete(entry); return false; }
    const char *name = json_string(entry, "name");
    const char *url = json_string(entry, "url_resolved");
    if (!url[0]) url = json_string(entry, "url");
    const char *codec = json_string(entry, "codec");
    // URLs need not end in .mp3/.aac; trust the declared codec. same_name()
    // ignores '+', so "AAC+" matches "AAC". The player re-checks the bytes.
    const bool playable = same_name(codec, "MP3") || same_name(codec, "AAC") ||
                          (!codec[0] && contains(url, ".mp3"));
    const bool accepted = name[0] && playable && enabled(entry, "lastcheckok", true) &&
        !enabled(entry, "hls", false) && !contains(url, ".m3u8") && !contains(url, ".m4a") &&
        (starts_with(url, "http://") || starts_with(url, "https://")) &&
        std::strlen(url) < sizeof(candidate.url);
    if (accepted) {
        copy_text(candidate.url, sizeof(candidate.url), url);
        copy_text(candidate.name, sizeof(candidate.name), name);
        copy_text(candidate.description, sizeof(candidate.description), description);
        candidate.frequency_decihz = parse_frequency(name);
    }
    cJSON_Delete(entry);
    return accepted;
}

struct DirectoryServers {
    char hosts[4][64] = {};
    std::size_t count = 0;
    mutable std::size_t preferred = 0;
};
DirectoryServers directory_servers(RadioCatalogCancelled stop) {
    DirectoryServers servers;
    if (cancelled(stop)) return servers;
    // Prefer virtual-host replica names first; raw IPs from service DNS often
    // reject IP Host headers or time out on DNS-filtering/proxy networks.
    copy_text(servers.hosts[servers.count++], sizeof(servers.hosts[0]), "de1.api.radio-browser.info");
    copy_text(servers.hosts[servers.count++], sizeof(servers.hosts[0]), "fi1.api.radio-browser.info");
    addrinfo hints = {}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("all.api.radio-browser.info", nullptr, &hints, &addresses) == 0) {
        for (const addrinfo *p = addresses; p && servers.count < 4; p = p->ai_next) {
            char ip[64] = {};
            const auto *addr = reinterpret_cast<const sockaddr_in *>(p->ai_addr);
            // Proxy fake-IP DNS answers (RFC 2544 range) are not service hosts.
            if ((ntohl(addr->sin_addr.s_addr) & 0xfffe0000u) == 0xc6120000u) continue;
            if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) continue;
            bool duplicate = false;
            for (std::size_t i = 0; i < servers.count; ++i)
                duplicate |= std::strcmp(servers.hosts[i], ip) == 0;
            if (!duplicate) copy_text(servers.hosts[servers.count++], sizeof(servers.hosts[0]), ip);
        }
        freeaddrinfo(addresses);
    }
    return servers;
}

template<class Emit>
bool query_directory(const DirectoryServers &servers,
                     const char *field, const char *value, const char *description,
                     RadioCatalogCancelled stop, Emit emit) {
    if (cancelled(stop) || (field && !value[0]) || !servers.count) return false;
    char filter[176] = {};  // "&name=<encoded>", empty for the whole country
    if (field) {
        const int prefix = std::snprintf(filter, sizeof(filter), "&%s=", field);
        if (prefix > 0 && static_cast<std::size_t>(prefix) < sizeof(filter))
            url_encode(value, filter + prefix, sizeof(filter) - prefix);
    }
    auto *object = static_cast<char *>(std::malloc(kObjectCapacity));
    if (!object) return false;
    bool complete = false;
    const std::size_t start = servers.preferred < servers.count ? servers.preferred : 0;
    for (std::size_t step = 0; step < servers.count && !cancelled(stop); ++step) {
        const std::size_t i = (start + step) % servers.count;
        char url[384];
        std::snprintf(url, sizeof(url),
            "http://%s/json/stations/search?countrycode=MY%s"
            "&hidebroken=true&order=clickcount&reverse=true",
            servers.hosts[i], filter);
        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = kHttpTimeoutMs;
        config.buffer_size = 1024;
        config.buffer_size_tx = 512;
        config.user_agent = "Passport-OS-MY-Radio/0.2.0-my.1";
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) break;
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        bool success = false;
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            if (esp_http_client_get_status_code(client) == 200) {
                RadioJsonStream parser(object, kObjectCapacity);
                char chunk[512];
                while (!cancelled(stop)) {
                    int n = esp_http_client_read(client, chunk, sizeof(chunk));
                    if (n <= 0) {
                        success = n == 0 && parser.Complete() && esp_http_client_is_complete_data_received(client);
                        break;
                    }
                    bool full = false;
                    parser.Feed(chunk, n, [&](const char *json) {
                        RadioStation row{};
                        if (!full && parse_station(json, description, row)) full = !emit(row);
                    });
                    if (full) { success = true; break; }
                }
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (success) {
            complete = true;
            servers.preferred = i;
            break; // an empty successful response is not a replica fault
        }
        ESP_LOGW(kTag, "Directory replica failed; trying another server");
    }
    std::free(object);
    return complete;
}

std::size_t discover_for_location(RadioStation *stations, std::size_t capacity,
                                 RadioLocation *location, RadioCatalogCancelled stop,
                                 const char *voice_station = nullptr) {
    if (cancelled(stop)) return 0;
    const CityPlace *city = find_city(location->search_city[0] ? location->search_city : location->city,
                                      location->region);
    if (city) {
        copy_text(location->search_city, sizeof(location->search_city), city->name);
        if (!location->region[0]) copy_text(location->region, sizeof(location->region), city->state);
    } else if (!location->region[0] && find_province(location->city)) {
        // A spoken state ("Selangor") is a region, not a city name.
        copy_text(location->region, sizeof(location->region), location->city);
    }
    const State *province = find_province(location->region);
    if (province) {
        copy_text(location->region, sizeof(location->region), province->name);
        copy_text(location->country_code, sizeof(location->country_code), "MY");
    }
    const DirectoryServers servers = directory_servers(stop);
    std::size_t count = 0;
    const auto voice_ready = [&] {
        if (!voice_station) return false;  // ordinary browsing keeps its full catalog
        for (std::size_t i = 0; i < count; ++i)
            if (!voice_station[0] || std::strstr(stations[i].name, voice_station)) return true;
        return false;
    };
    const auto local = [&](const RadioStation &row) {
        if (!duplicate_url(stations, count, row.url)) stations[count++] = row;
        return count < capacity;
    };
    const char *query = location->search_city[0] ? location->search_city : location->city;
    query_directory(servers, "name", query, "Local · Online", stop, local);
    if (!voice_ready() && province && count < capacity && !same_name(query, province->name)) {
        query_directory(servers, "name", province->name, "State · Online", stop, local);
    }
    if (cancelled(stop)) return 0;
    ESP_LOGI(kTag, "Catalog %s / %s: %u stations", location->city, location->region, (unsigned)count);
    return count;
}

#ifdef ESP_PLATFORM
struct CatalogCache {
    static constexpr uint32_t kMagic = 0x4d594c32;  // MY local priority rows, version 2
    static constexpr std::size_t kMaxStations = 8;
    static constexpr int64_t kTtlUs = 30LL * 60 * 1000 * 1000;
    uint32_t magic;
    uint32_t check;
    int64_t saved_us;
    std::size_t count;
    RadioLocation preferred;
    RadioLocation resolved;
    RadioStation stations[kMaxStations];

    uint32_t checksum() const {
        uint32_t hash = 2166136261u ^ kMagic;
        const auto mix = [&](const void *data, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i)
                hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
        };
        mix(&saved_us, sizeof(saved_us));
        mix(&count, sizeof(count));
        mix(&preferred, sizeof(preferred));
        mix(&resolved, sizeof(resolved));
        mix(stations, sizeof(stations));
        return hash;
    }

    static bool same_location(const RadioLocation &a, const RadioLocation &b) {
        return !std::strcmp(a.city, b.city) && !std::strcmp(a.region, b.region) &&
               !std::strcmp(a.search_city, b.search_city) &&
               !std::strcmp(a.country_code, b.country_code);
    }

    bool load(const RadioLocation &want, RadioStation *out, std::size_t capacity,
              RadioLocation *location, const char *voice_station) {
        if (magic != kMagic || !count || count > kMaxStations || capacity > kMaxStations ||
            check != checksum()) {
            magic = 0;
            return false;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us < saved_us) {
            if (esp_reset_reason() == ESP_RST_SW) {
                saved_us = now_us;
                check = checksum();
            } else {
                magic = 0;
                return false;
            }
        } else if (now_us - saved_us > kTtlUs) {
            magic = 0;
            return false;
        }
        if (!same_location(want, preferred)) return false;
        if (voice_station && voice_station[0]) {
            bool matched = false;
            for (std::size_t i = 0; i < count; ++i)
                matched |= std::strstr(stations[i].name, voice_station) != nullptr;
            if (!matched) return false;
        }
        const std::size_t n = std::min(capacity, count);
        for (std::size_t i = 0; i < n; ++i) out[i] = stations[i];
        *location = resolved;
        ESP_LOGI(kTag, "Catalog cache hit %s / %s: %u stations", location->city, location->region, (unsigned)n);
        return true;
    }

    void save(const RadioLocation &want, const RadioLocation &loc,
              const RadioStation *rows, std::size_t n) {
        if (!n || n > kMaxStations) return;
        *this = {};
        preferred = want;
        resolved = loc;
        count = n;
        for (std::size_t i = 0; i < n; ++i) stations[i] = rows[i];
        saved_us = esp_timer_get_time();
        check = checksum();
        magic = kMagic;
    }
};
RTC_NOINIT_ATTR CatalogCache s_catalog_cache;
#endif
} // namespace

std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                  RadioLocation *location, RadioCatalogCancelled stop) {
    if (!stations || !capacity || !location || cancelled(stop)) return 0;
    *location = {};
    if (!locate(location, stop)) {
        if (cancelled(stop)) return 0;
        // Default city when IP location fails.
        copy_text(location->country_code, sizeof(location->country_code), "MY");
        copy_text(location->city, sizeof(location->city), "Kuala Lumpur");
    }
    return discover_for_location(stations, capacity, location, stop);
}

std::size_t radio_catalog_discover_location(const RadioLocation &preferred,
                                           RadioStation *stations, std::size_t capacity,
                                           RadioLocation *location, RadioCatalogCancelled stop,
                                           const char *voice_station) {
    if (!stations || !capacity || !location || cancelled(stop)) return 0;
    if (!preferred.city[0]) return radio_catalog_discover(stations, capacity, location, stop);
    *location = preferred;
    if (!location->country_code[0]) copy_text(location->country_code, sizeof(location->country_code), "MY");
#ifdef ESP_PLATFORM
    const RadioLocation want = *location;
    if (s_catalog_cache.load(want, stations, capacity, location, voice_station)) {
        return std::min(capacity, s_catalog_cache.count);
    }
#endif
    const std::size_t found = discover_for_location(stations, capacity, location, stop, voice_station);
#ifdef ESP_PLATFORM
    if (found > 0 && !cancelled(stop) &&
        (!voice_station || found >= CatalogCache::kMaxStations)) {
        s_catalog_cache.save(want, *location, stations, found);
    }
#endif
    return found;
}

bool radio_catalog_known_place(const char *name) {
    return name && name[0] && (find_city(name, "") || find_province(name));
}

std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                      std::size_t capacity, RadioLocation *location) {
    RadioLocation preferred = {};
    if (city) copy_text(preferred.city, sizeof(preferred.city), city);
    return radio_catalog_discover_location(preferred, stations, capacity, location);
}

bool radio_catalog_discover_all(const RadioLocation &preferred, RadioStationList &list,
                                RadioLocation *location, RadioCatalogCancelled stop,
                                const char *voice_station) {
    if (!location || cancelled(stop)) return false;
    // RTC holds only a short city/state prefix. Nationwide discovery always
    // runs, including after cache hits and voice requests.
    constexpr std::size_t local_capacity = 8;
    auto *local = static_cast<RadioStation *>(std::calloc(local_capacity, sizeof(RadioStation)));
    bool ok = true;
    if (local) {
        const auto n = radio_catalog_discover_location(preferred, local, local_capacity,
                                                      location, stop, voice_station);
        for (std::size_t i = 0; i < n && ok; ++i) ok = list.append(local[i]);
        std::free(local);
    }
    const auto append = [&](const RadioStation &row) {
        if (!list.contains_url(row.url)) ok = list.append(row);
        return ok;
    };
    for (const auto &preset : kRadioPresets) {
        if (!ok || cancelled(stop)) break;
        append(preset);
    }
    const bool complete = ok && !cancelled(stop) &&
        query_directory(directory_servers(stop), nullptr, "", "Malaysia · Online", stop, append);
    if (!ok || cancelled(stop)) list.clear();
    return complete && ok && !cancelled(stop);
}
