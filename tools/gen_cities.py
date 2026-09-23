#!/usr/bin/env python3
"""Turn Natural Earth's populated places into a C table for offline reverse geocoding.

    python tools/gen_cities.py                       # empty placeholder table
    python tools/gen_cities.py ne_10m_populated_places_simple.geojson
    python tools/gen_cities.py places.csv --min-pop 50000

Writes src/core/geo_city_table.c. **Nothing in the build runs this** -- the same trap
tools/gen_assets.py documents -- so after running it, check the artefact rather than the exit
code, and rebuild.

WHY NATURAL EARTH AND NOT GEONAMES
    `ne_10m_populated_places` is public domain. GeoNames `cities15000` has better coverage of
    smaller towns and a ready-made ASCII column, and is CC BY 4.0. A line of text in a photo
    frame's margin is not worth taking on an attribution obligation for, particularly while
    ticket 65 is removing one.

    **THERE IS NO geoCSV DIRECTORY in natural-earth-vector, and this docstring said there was**
    until 2026-09-20 -- that URL 404s. What the repository carries is GeoJSON and shapefiles, so
    take:

        geojson/ne_10m_populated_places_simple.geojson      4.9 MB, 7342 features

    The `_simple` variant is the one to use: the full `ne_10m_populated_places.geojson` is 19 MB
    and the shapefile's .dbf is 48 MB, almost all of it localised name columns nothing here reads.

    Fields used: NAMEASCII (falling back to NAME), ADM0NAME (falling back to SOV0NAME), LATITUDE,
    LONGITUDE, POP_MAX. **GeoJSON spells them lowercase and a CSV export spells them upper**, so
    source_rows() upper-cases every key and the rest of this file sees one spelling.

    **And the name blob cannot pass 65,535 bytes**, because `s_name_off` is `uint16_t`. The
    `off > 0xFFFF` check below refuses loudly rather than truncating, so a `--min-pop` low enough to
    overflow it fails the run instead of emitting a table whose later names point at the wrong bytes.
    The shipping table is `--min-pop 1000`, stamped in the generated file's first line, and that is
    about the most that fits.

WHAT THE FILTERING IS FOR
    The panel's font is 47 hand-drawn glyphs: space, `. - : /`, digits, A-Z and a-f
    (src/core/epd_text.c). A character outside that set draws a hollow box, deliberately, so a
    name has to be reduced to the set HERE rather than discovered on the glass. Accents are
    folded (ZURICH, SAO PAULO), everything else outside the set is dropped, and a name left
    empty by that is skipped rather than emitted blank.
"""

import argparse
import csv
import json
import pathlib
import sys
import unicodedata

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_C = REPO_ROOT / "src" / "core" / "geo_city_table.c"

# Exactly what epd_text.c can draw, once a name is uppercased.
ALLOWED = set(" .-:/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")

# uint8_t country index, so 255 countries is the ceiling. Natural Earth has ~200 sovereignties in
# this file, so the assert below is a tripwire rather than an expected failure.
MAX_COUNTRIES = 255


def fold(s):
    """Uppercase ASCII containing only characters the panel's font has."""
    if not s:
        return ""
    # NFKD splits an accented letter into letter + combining mark; dropping non-ASCII then leaves
    # the letter. This is what turns Zurich and Sao Paulo into something drawable.
    flat = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii")
    out = "".join(ch for ch in flat.upper() if ch in ALLOWED)
    # Collapse the runs of spaces that dropping a character can leave behind.
    return " ".join(out.split())


def source_rows(path):
    """Property dicts from either input, every key upper-cased.

    GeoJSON because that is what natural-earth-vector actually has; CSV because an export or a
    hand-trimmed subset is the obvious thing to reach for and costs nothing to keep working.
    """
    if pathlib.Path(path).suffix.lower() in (".geojson", ".json"):
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        for feat in doc.get("features", []):
            props = {str(k).upper(): v for k, v in (feat.get("properties") or {}).items()}
            # A Point's own coordinates stand in for the LATITUDE/LONGITUDE properties, which
            # Natural Earth carries but a trimmed export might not.
            geom = feat.get("geometry") or {}
            coords = geom.get("coordinates")
            if geom.get("type") == "Point" and isinstance(coords, list) and len(coords) >= 2:
                props.setdefault("LONGITUDE", coords[0])
                props.setdefault("LATITUDE", coords[1])
            yield props
        return
    with open(path, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            yield {str(k or "").upper(): v for k, v in r.items()}


def read_places(path, min_pop):
    rows = {}
    if True:
        for r in source_rows(path):
            try:
                pop = int(float(r.get("POP_MAX") or 0))
                lat = float(r["LATITUDE"])
                lon = float(r["LONGITUDE"])
            except (KeyError, TypeError, ValueError):
                continue
            if pop < min_pop:
                continue
            name = fold(r.get("NAMEASCII") or r.get("NAME") or "")
            country = fold(r.get("ADM0NAME") or r.get("SOV0NAME") or "")
            if not name or not country:
                continue
            if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
                continue
            # One entry per name-and-country, keeping the largest. Natural Earth carries several
            # records for one metropolis (the city, the seat of government, the metro area) and
            # three TOKYOs in the table would only make the scan longer.
            key = (name, country)
            if key not in rows or pop > rows[key][0]:
                rows[key] = (pop, lat, lon)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?",
                    help="ne_10m_populated_places_simple.geojson (or a CSV export); "
                         "omit for an empty table")
    ap.add_argument("--min-pop", type=int, default=100000,
                    help="smallest POP_MAX to keep (default 100000)")
    args = ap.parse_args()

    if args.csv is None:
        places = {}
        source = "none -- placeholder, run this script with a CSV to fill it"
    else:
        places = read_places(args.csv, args.min_pop)
        source = "%s, POP_MAX >= %d" % (pathlib.Path(args.csv).name, args.min_pop)
        if not places:
            print("no usable rows in %s" % args.csv, file=sys.stderr)
            return 1

    # Sorted so the generated file diffs stably when the source data is refreshed. The scan is
    # linear, so the order carries no meaning beyond that.
    items = sorted(places.items(), key=lambda kv: (kv[0][1], kv[0][0]))

    countries = sorted({country for (_name, country) in places})
    if len(countries) > MAX_COUNTRIES:
        print("%d countries exceeds the uint8_t index" % len(countries), file=sys.stderr)
        return 1
    country_index = {c: i for i, c in enumerate(countries)}

    names_blob = []
    name_off = []
    off = 0
    lat_q = []
    lon_q = []
    country_q = []
    for (name, country), (_pop, lat, lon) in items:
        name_off.append(off)
        names_blob.append(name)
        off += len(name) + 1
        lat_q.append(int(round(lat * 100.0)))
        lon_q.append(int(round(lon * 100.0)))
        country_q.append(country_index[country])
    if off > 0xFFFF:
        print("name blob is %d bytes, past the uint16_t offsets" % off, file=sys.stderr)
        return 1

    def rows_of(values, per_line, fmt="%d"):
        out = []
        for i in range(0, len(values), per_line):
            out.append("    " + ", ".join(fmt % v for v in values[i:i + per_line]) + ",")
        return "\n".join(out) if out else "    0,"

    def c_string(s):
        return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')

    body = []
    body.append("// GENERATED by tools/gen_cities.py -- do not edit. Source: %s\n" % source)
    body.append("//")
    body.append("// Natural Earth is public domain (https://www.naturalearthdata.com/about/terms-of-use/),")
    body.append("// which is why it was chosen over GeoNames' CC BY 4.0 -- see src/core/geo_city.h.")
    body.append("")
    body.append('#include "geo_city.h"')
    body.append("")
    body.append("#define CITY_COUNT %d" % len(items))
    body.append("")

    if items:
        body.append("static const int16_t s_lat[CITY_COUNT] = {")
        body.append(rows_of(lat_q, 16))
        body.append("};")
        body.append("")
        body.append("static const int16_t s_lon[CITY_COUNT] = {")
        body.append(rows_of(lon_q, 16))
        body.append("};")
        body.append("")
        body.append("static const uint8_t s_country[CITY_COUNT] = {")
        body.append(rows_of(country_q, 24))
        body.append("};")
        body.append("")
        body.append("static const uint16_t s_name_off[CITY_COUNT] = {")
        body.append(rows_of(name_off, 16))
        body.append("};")
        body.append("")
        # One string literal per name, concatenated with explicit NULs, so the offsets above are
        # checkable by eye against the source. A single giant literal would be shorter and
        # unreadable.
        body.append("static const char s_names[] =")
        for name in names_blob:
            body.append("    %s \"\\0\"" % c_string(name))
        body.append("    ;")
        body.append("")
        body.append("static const char *const s_countries[] = {")
        for c in countries:
            body.append("    %s," % c_string(c))
        body.append("};")
        body.append("")
        body.append("static const geo_table_t s_table = {")
        body.append("    s_lat, s_lon, s_country, s_name_off, s_names, s_countries,")
        body.append("    CITY_COUNT, %d," % len(countries))
        body.append("};")
    else:
        body.append("// No data generated yet: the lookup returns false and the frame simply draws")
        body.append("// no location. Run tools/gen_cities.py with a CSV to fill this in.")
        body.append("static const geo_table_t s_table = {")
        body.append("    NULL, NULL, NULL, NULL, NULL, NULL, 0, 0,")
        body.append("};")

    body.append("")
    body.append("const geo_table_t *geo_city_table(void)")
    body.append("{")
    body.append("    return &s_table;")
    body.append("}")
    body.append("")

    OUT_C.write_text("\n".join(body), encoding="utf-8", newline="\n")
    print("wrote %s: %d cities, %d countries, %d bytes of names" %
          (OUT_C.relative_to(REPO_ROOT), len(items), len(countries), off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
