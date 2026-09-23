#include "geo_city.h"

#include <math.h>
#include <stddef.h>

// Mean kilometres per degree of latitude. The equirectangular approximation below is wrong by a
// fraction of a percent at city scale, and the answer it produces is a NAME -- an error would have
// to move the query across tens of kilometres to change which name comes back.
#define KM_PER_DEG 111.32f
#define DEG_PER_RAD 0.017453292f

bool geo_city_nearest(const geo_table_t *t, int32_t lat_udeg, int32_t lon_udeg, int32_t max_km,
                      const char **out_city, const char **out_country)
{
    if (out_city != NULL) {
        *out_city = NULL;
    }
    if (out_country != NULL) {
        *out_country = NULL;
    }
    if (t == NULL || t->count <= 0 || t->lat == NULL || t->lon == NULL || t->names == NULL ||
        t->name_off == NULL || max_km <= 0) {
        return false;
    }

    const float qlat = (float)lat_udeg / 1000000.0f;
    const float qlon = (float)lon_udeg / 1000000.0f;

    // **One cosine for the whole scan**, which is the reason this is affordable: a haversine per
    // record would be 5,000 transcendentals per photograph for an answer that is a place name. A
    // degree of longitude shrinks with latitude and a degree of latitude does not, so scaling the
    // longitude difference by cos(lat) makes the two comparable and the comparison a plain
    // Euclidean one.
    const float shrink = cosf(qlat * DEG_PER_RAD);

    const float limit = (float)max_km / KM_PER_DEG;
    float best = limit * limit;
    int32_t found = -1;

    for (int32_t i = 0; i < t->count; i++) {
        const float dy = (float)t->lat[i] / 100.0f - qlat;
        float dlon = (float)t->lon[i] / 100.0f - qlon;
        // Across the antimeridian the raw difference is ~360 and the nearest city is missed. Two
        // comparisons, and they are the difference between naming Suva and naming nothing.
        if (dlon > 180.0f) {
            dlon -= 360.0f;
        } else if (dlon < -180.0f) {
            dlon += 360.0f;
        }
        const float dx = dlon * shrink;
        const float d2 = dy * dy + dx * dx;
        if (d2 < best) {
            best = d2;
            found = i;
        }
    }

    if (found < 0) {
        return false;
    }
    if (out_city != NULL) {
        *out_city = t->names + t->name_off[found];
    }
    if (out_country != NULL && t->country != NULL && t->country_names != NULL) {
        const uint8_t idx = t->country[found];
        if ((int32_t)idx < t->country_count) {
            *out_country = t->country_names[idx];
        }
    }
    return true;
}
