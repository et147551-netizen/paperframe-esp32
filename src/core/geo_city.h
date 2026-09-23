// Offline reverse geocoding, city level, ASCII, uppercase.
//
// A photograph's GPS tag turned into `TOKYO JAPAN` for the matte band (ticket 64), with **no API
// and no network**. The owner's constraint is what makes that affordable: city level and ASCII
// only. Street-level accuracy would need a database nobody puts on a photo frame, and local place
// names would need a CJK font -- a glyph table with a licence attached, which is exactly what
// ticket 65 is unwinding.
//
// **Cheaper than the alternatives on the axis that matters here.** The table is `.rodata`, so it is
// memory-mapped from flash and costs no internal RAM at all; a weather API over TLS costs 45-47 KB
// of it (measured, ticket 60), and internal RAM is this board's scarce resource. It is also better
// on privacy than any API route: nothing leaves the frame, and the caller keeps the NAME rather
// than the coordinates -- "derive before you retain", the owner's instruction of 2026-09-12.
//
// **The data is Natural Earth's `ne_10m_populated_places`, public domain.** GeoNames `cities15000`
// has better coverage and an explicit ASCII column, and is CC BY 4.0; an attribution obligation is
// not worth taking on for a line of text while ticket 65 is removing one. `tools/gen_cities.py`
// turns the download into `geo_city_table.c` and **nothing in the build runs it**, so check for the
// artefact rather than the exit code (`docs/build-system.md`).

#ifndef GEO_CITY_H
#define GEO_CITY_H

#include <stdbool.h>
#include <stddef.h> // NULL, which the generated empty table uses
#include <stdint.h>

// Parallel arrays rather than an array of structs, and it is not style: the scan reads only `lat`
// and `lon`, so keeping them contiguous is one cache line per eight candidates instead of one per
// candidate. A packed 5-byte struct would need an attribute and would still interleave the fields
// the scan never touches.
typedef struct {
    const int16_t *lat;     // degrees x 100. +-9000, so 0.01 deg = 1.1 km -- finer than city level
    const int16_t *lon;     // degrees x 100. +-18000
    const uint8_t *country; // index into `country_names`
    const uint16_t *name_off; // byte offset of this city's name in `names`
    const char *names;        // NUL-terminated ASCII, uppercase, concatenated
    const char *const *country_names;
    int32_t count;
    int32_t country_count;
} geo_table_t;

// The generated table. `count` is 0 when `tools/gen_cities.py` has not been run with real data,
// which is a working build with the feature inert rather than a link error.
const geo_table_t *geo_city_table(void);

// The nearest city to a position, or false when the table is empty or nothing is within `max_km`.
//
// **The bound is what keeps it honest.** Nearest-city has no notion of "nowhere": a photograph
// taken at sea, in a desert or on a mountain would otherwise be labelled with a city 200 km away,
// stated as fact. Beyond the bound the caller writes no location at all -- not a country, because
// a city table cannot supply one for a point with no city near it, and a coarse country grid is a
// second dataset for one fallback line.
//
// `*out_city` and `*out_country` point into the table itself, so they are flash-resident, valid
// forever and need no buffer. `out_country` may be NULL if the caller does not want it.
bool geo_city_nearest(const geo_table_t *t, int32_t lat_udeg, int32_t lon_udeg, int32_t max_km,
                      const char **out_city, const char **out_country);

// The default bound, in kilometres. A city's own extent plus its suburbs is tens of kilometres, and
// 100 km is about the point at which a name stops being a description of where the photograph was
// taken. Not measured -- there is nothing to measure -- so it is a judgement, stated once.
#define GEO_CITY_MAX_KM 100

#endif // GEO_CITY_H
