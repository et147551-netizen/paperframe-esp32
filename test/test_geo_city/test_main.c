// Host-side tests for the offline reverse geocoder.
//
// Against a FIXTURE table of five cities, not the generated one: a test that read
// geo_city_table.c would change its answers every time the data was refreshed, and would pass
// vacuously today because the committed table is the empty placeholder.
//
// The distances are worked out by hand from 111.32 km per degree, so each case states how far the
// query is from its answer and why that is inside or outside the bound.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "geo_city.h"

void setUp(void) {}
void tearDown(void) {}

// Sorted by country then name, the way tools/gen_cities.py emits them.
//                                  TAVEUNI  KAWASAKI  TOKYO   TAIPEI  LONDON
static const int16_t FX_LAT[5] = {    -1685,     3553,  3569,    2504,   5151};
static const int16_t FX_LON[5] = {    17990,    13970, 13969,   12156,    -13};
static const uint8_t FX_COUNTRY[5] = {     0,        1,     1,       2,      3};
static const uint16_t FX_OFF[5] = {        0,        8,    17,      23,     30};
static const char FX_NAMES[] = "TAVEUNI\0KAWASAKI\0TOKYO\0TAIPEI\0LONDON\0";
static const char *const FX_COUNTRIES[4] = {"FIJI", "JAPAN", "TAIWAN", "UNITED KINGDOM"};

static const geo_table_t FX = {
    FX_LAT, FX_LON, FX_COUNTRY, FX_OFF, FX_NAMES, FX_COUNTRIES, 5, 4,
};

// Degrees to the microdegrees the EXIF reader produces.
static int32_t ud(double deg)
{
    return (int32_t)(deg * 1000000.0);
}

static void test_the_offsets_address_the_names_they_claim_to(void)
{
    // Proof-reading the fixture itself, because every assertion below rests on it.
    TEST_ASSERT_EQUAL_STRING("TAVEUNI", FX_NAMES + FX_OFF[0]);
    TEST_ASSERT_EQUAL_STRING("KAWASAKI", FX_NAMES + FX_OFF[1]);
    TEST_ASSERT_EQUAL_STRING("TOKYO", FX_NAMES + FX_OFF[2]);
    TEST_ASSERT_EQUAL_STRING("TAIPEI", FX_NAMES + FX_OFF[3]);
    TEST_ASSERT_EQUAL_STRING("LONDON", FX_NAMES + FX_OFF[4]);
}

static void test_a_position_on_the_city_names_it(void)
{
    const char *city = NULL;
    const char *country = NULL;
    TEST_ASSERT_TRUE(
        geo_city_nearest(&FX, ud(35.69), ud(139.69), GEO_CITY_MAX_KM, &city, &country));
    TEST_ASSERT_EQUAL_STRING("TOKYO", city);
    TEST_ASSERT_EQUAL_STRING("JAPAN", country);
}

// 0.2695 degrees north of Tokyo is 30 km, well inside the bound, and 2,100 km from the next
// fixture -- so this tests the bound rather than the tie-breaking.
static void test_thirty_kilometres_away_still_names_the_city(void)
{
    const char *city = NULL;
    TEST_ASSERT_TRUE(
        geo_city_nearest(&FX, ud(35.9595), ud(139.69), GEO_CITY_MAX_KM, &city, NULL));
    TEST_ASSERT_EQUAL_STRING("TOKYO", city);
}

// Two cities 18 km apart, queried from 2 km outside one of them. If the scan kept the first match
// rather than the nearest, or compared the wrong axis, this returns TOKYO.
static void test_the_nearest_of_two_wins(void)
{
    const char *city = NULL;
    TEST_ASSERT_TRUE(geo_city_nearest(&FX, ud(35.55), ud(139.70), GEO_CITY_MAX_KM, &city, NULL));
    TEST_ASSERT_EQUAL_STRING("KAWASAKI", city);
}

// **The bound is the honesty of the whole feature.** A photograph taken at sea would otherwise be
// labelled with a city thousands of kilometres away, stated as fact. The nearest fixture to this
// point in the Pacific is 4,800 km off.
static void test_nowhere_is_nowhere(void)
{
    const char *city = "unset";
    const char *country = "unset";
    TEST_ASSERT_FALSE(geo_city_nearest(&FX, ud(0.0), ud(-140.0), GEO_CITY_MAX_KM, &city, &country));
    TEST_ASSERT_NULL(city);
    TEST_ASSERT_NULL(country);
}

// 150 km from Tokyo: out at the default bound, in at a wider one. The same query both ways, so the
// bound is what moves and not the arithmetic.
static void test_the_bound_is_the_bound(void)
{
    const char *city = NULL;
    TEST_ASSERT_FALSE(geo_city_nearest(&FX, ud(37.0375), ud(139.69), 100, &city, NULL));
    TEST_ASSERT_TRUE(geo_city_nearest(&FX, ud(37.0375), ud(139.69), 200, &city, NULL));
    TEST_ASSERT_EQUAL_STRING("TOKYO", city);
}

// A tenth of a degree east of the date line, answered by a city a tenth of a degree west of it.
// The raw longitude difference is 359.8 degrees; without the wrap this returns false, and a
// photograph taken in Fiji is labelled nothing at all.
static void test_across_the_antimeridian(void)
{
    const char *city = NULL;
    const char *country = NULL;
    TEST_ASSERT_TRUE(
        geo_city_nearest(&FX, ud(-16.85), ud(-179.90), GEO_CITY_MAX_KM, &city, &country));
    TEST_ASSERT_EQUAL_STRING("TAVEUNI", city);
    TEST_ASSERT_EQUAL_STRING("FIJI", country);
}

// A degree of longitude is 111 km at the equator and 69 km at London's latitude. Ignoring that
// would make everything east and west of a northern city look further away than it is: this query
// is 0.5 degrees of longitude from London, which is 34 km there and would be 56 km unscaled.
static void test_longitude_shrinks_with_latitude(void)
{
    const char *city = NULL;
    TEST_ASSERT_TRUE(geo_city_nearest(&FX, ud(51.51), ud(0.37), 40, &city, NULL));
    TEST_ASSERT_EQUAL_STRING("LONDON", city);
}

static void test_an_empty_table_names_nothing(void)
{
    const geo_table_t empty = {NULL, NULL, NULL, NULL, NULL, NULL, 0, 0};
    const char *city = "unset";
    TEST_ASSERT_FALSE(geo_city_nearest(&empty, ud(35.69), ud(139.69), GEO_CITY_MAX_KM, &city, NULL));
    TEST_ASSERT_NULL(city);

    TEST_ASSERT_FALSE(geo_city_nearest(NULL, 0, 0, GEO_CITY_MAX_KM, &city, NULL));
    TEST_ASSERT_FALSE(geo_city_nearest(&FX, ud(35.69), ud(139.69), 0, &city, NULL));
    // No out parameters at all is allowed: a caller may only want to know whether there is one.
    TEST_ASSERT_TRUE(geo_city_nearest(&FX, ud(35.69), ud(139.69), GEO_CITY_MAX_KM, NULL, NULL));
}

// The generated table links and is reachable. Its contents are deliberately not asserted -- it is
// the empty placeholder until somebody runs tools/gen_cities.py with the download, and a test that
// expected data would have to be edited the day it arrives.
static void test_the_generated_table_is_linked(void)
{
    const geo_table_t *t = geo_city_table();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, t->count);
    if (t->count == 0) {
        const char *city = NULL;
        TEST_ASSERT_FALSE(geo_city_nearest(t, ud(35.69), ud(139.69), GEO_CITY_MAX_KM, &city, NULL));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_offsets_address_the_names_they_claim_to);
    RUN_TEST(test_a_position_on_the_city_names_it);
    RUN_TEST(test_thirty_kilometres_away_still_names_the_city);
    RUN_TEST(test_the_nearest_of_two_wins);
    RUN_TEST(test_nowhere_is_nowhere);
    RUN_TEST(test_the_bound_is_the_bound);
    RUN_TEST(test_across_the_antimeridian);
    RUN_TEST(test_longitude_shrinks_with_latitude);
    RUN_TEST(test_an_empty_table_names_nothing);
    RUN_TEST(test_the_generated_table_is_linked);
    return UNITY_END();
}
