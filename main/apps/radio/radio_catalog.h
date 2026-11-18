#pragma once

#include "radio_station_list.h"

#include <cstddef>

struct RadioLocation {
    char country_code[4];
    char region[48];
    char city[48];
    char search_city[48];
};

using RadioCatalogCancelled = bool (*)();

std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location,
                                   RadioCatalogCancelled cancelled = nullptr);

// True for a Malaysian place or state name in the offline catalog (case and
// punctuation ignored); used to validate voice requests.
bool radio_catalog_known_place(const char *name);

// Loads the most popular playable stations for a user-selected Malaysian city.
// This skips IP geolocation and is used by the on-device city selector.
std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location);

// Preferred profile location skips IP geolocation; empty city uses IP instead.
// These rows prioritize city and state stations at the front of the full list.
// Voice requests may pass station keywords (including "" for any local station):
// stop directory lookups once that station is found, then load the nationwide list separately.
std::size_t radio_catalog_discover_location(const RadioLocation &preferred,
                                           RadioStation *stations, std::size_t capacity,
                                           RadioLocation *location,
                                           RadioCatalogCancelled cancelled = nullptr,
                                           const char *voice_station = nullptr);

// Returns true after a complete nationwide response. On a directory outage the
// list still contains the local rows and built-in streams available to play.
bool radio_catalog_discover_all(const RadioLocation &preferred, RadioStationList &list,
                                RadioLocation *location, RadioCatalogCancelled cancelled,
                                const char *voice_station = nullptr);
