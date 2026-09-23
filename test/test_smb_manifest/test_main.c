// Host tests for the mirror's index and naming rules (ticket 25).
//
// Non-ASCII names are written as explicit \x escapes rather than as literal UTF-8. This
// machine's default codepage is cp932 and several tools in this project have silently
// re-encoded a source file already; an escape sequence cannot be re-encoded.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "smb_manifest.h"

static char s_arena[8192];
static smb_manifest_t s_m;

void setUp(void)
{
    smb_manifest_init(&s_m, s_arena, sizeof(s_arena));
}

void tearDown(void) {}

// ---------------------------------------------------------------------- naming

static void test_ascii_leaf_is_kept_verbatim(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("20260511/photo1.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("photo1.jpg", out);
}

static void test_case_is_preserved_on_a_verbatim_name(void)
{
    // The extension is matched case-insensitively, but a name FAT can hold is copied as
    // it is -- lowercasing it would make the local file a different file from the one on
    // the share for no reason.
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("dir/IMG_0042.JPG", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("IMG_0042.JPG", out);
}

static void test_all_four_extensions_are_accepted(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("a.jpg", out, sizeof(out)));
    TEST_ASSERT_TRUE(smb_manifest_local_name("a.jpeg", out, sizeof(out)));
    TEST_ASSERT_TRUE(smb_manifest_local_name("a.png", out, sizeof(out)));
    TEST_ASSERT_TRUE(smb_manifest_local_name("a.bmp", out, sizeof(out)));
}

static void test_non_image_extensions_are_rejected(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_FALSE(smb_manifest_local_name("dir/readme.txt", out, sizeof(out)));
    TEST_ASSERT_FALSE(smb_manifest_local_name("dir/noextension", out, sizeof(out)));
    TEST_ASSERT_FALSE(smb_manifest_local_name("dir/trailingdot.", out, sizeof(out)));
}

static void test_a_dotfile_is_not_a_photograph(void)
{
    // Same rule as photo_list_is_image(): a leading dot leaves an empty stem.
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_FALSE(smb_manifest_local_name("dir/.jpg", out, sizeof(out)));
}

// Ticket 38, and a departure from FR-4.6 rather than a tightening of it: the mirror rejects
// EVERY leading-dot leaf, not only one that is nothing but an extension. The case is
// Android MediaStore's soft-deleted photographs, twelve of which sit on this bench's share
// and used to pass -- strrchr finds the last dot, so the stem is non-empty and the old test
// had nothing to say about them.
static void test_a_leading_dot_is_rejected_however_late_the_extension(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_FALSE(smb_manifest_local_name(
        "202606/.trashed-1783782710-album_v27_1780152293232.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(smb_manifest_local_name("dir/..jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(smb_manifest_local_name(".hidden.png", out, sizeof(out)));

    // The dot has to be in the LEAF. A dotted directory says nothing about the file in it,
    // and rejecting on the whole path would have been the easy way to get this wrong.
    TEST_ASSERT_TRUE(smb_manifest_local_name(".album/photo1.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("photo1.jpg", out);

    // The control: the same name without the dot is still a photograph, so the test cannot
    // pass by rejecting everything.
    TEST_ASSERT_TRUE(smb_manifest_local_name(
        "202606/trashed-1783782710-album_v27_1780152293232.jpg", out, sizeof(out)));
}

static void test_non_ascii_leaf_becomes_a_hash(void)
{
    // "\xE6\x97\xA5\xE6\x9C\xAC.jpg" is UTF-8 for U+65E5 U+672C ("nihon") plus .jpg.
    // /data is FAT/CP437 and cannot store it.
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(
        smb_manifest_local_name("20260511/\xE6\x97\xA5\xE6\x9C\xAC.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(16, (int)strlen(out)); // smb_ + 8 hex + . + jpg
    TEST_ASSERT_EQUAL_STRING_LEN("smb_", out, 4);
    TEST_ASSERT_EQUAL_STRING(".jpg", out + 12);
}

static void test_a_hostile_character_becomes_a_hash(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("dir/who?.png", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING_LEN("smb_", out, 4);
    TEST_ASSERT_EQUAL_STRING(".png", out + 12);
}

static void test_an_overlong_ascii_name_becomes_a_hash(void)
{
    char leaf[SMB_MANIFEST_NAME_SIZE + 40];
    memset(leaf, 'a', sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = '\0';
    memcpy(leaf + sizeof(leaf) - 5, ".jpg", 5);

    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name(leaf, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING_LEN("smb_", out, 4);
}

static void test_the_hash_covers_the_path_not_the_leaf(void)
{
    // The whole reason the hash is not of the leaf: two directories, one filename. If
    // this ever regresses, the second file silently overwrites the first in /data.
    char a[SMB_MANIFEST_NAME_SIZE], b[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("2025/\xE6\x97\xA5.jpg", a, sizeof(a)));
    TEST_ASSERT_TRUE(smb_manifest_local_name("2026/\xE6\x97\xA5.jpg", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING(".jpg", a + 12);
    TEST_ASSERT_EQUAL_STRING(".jpg", b + 12);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

static void test_the_hashed_extension_is_lowercased(void)
{
    char out[SMB_MANIFEST_NAME_SIZE];
    TEST_ASSERT_TRUE(smb_manifest_local_name("dir/\xE6\x97\xA5.JPEG", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".jpeg", out + 12);
}

// ------------------------------------------------------------------- the record

static void test_add_find_and_change_detection(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1234, 99));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)smb_manifest_count(&s_m));
    TEST_ASSERT_EQUAL_STRING("photo1.jpg", smb_manifest_local(&s_m, 0));
    TEST_ASSERT_EQUAL_STRING("20260511/photo1.jpg", smb_manifest_path(&s_m, 0));

    const int i = smb_manifest_find(&s_m, "20260511/photo1.jpg");
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_EQUAL_INT(-1, smb_manifest_find(&s_m, "20260511/other.jpg"));

    TEST_ASSERT_TRUE(smb_manifest_unchanged(&s_m, 0, 1234, 99));
    TEST_ASSERT_FALSE(smb_manifest_unchanged(&s_m, 0, 1235, 99)); // resized
    TEST_ASSERT_FALSE(smb_manifest_unchanged(&s_m, 0, 1234, 100)); // touched
}

static void test_a_full_arena_rejects_cleanly(void)
{
    // A half-added record would leave count and the arena disagreeing.
    char tiny[16];
    smb_manifest_t m;
    TEST_ASSERT_TRUE(smb_manifest_init(&m, tiny, sizeof(tiny)));
    TEST_ASSERT_FALSE(smb_manifest_add(&m, "photo1.jpg", "a/very/long/path.jpg", 1, 2));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)smb_manifest_count(&m));
    TEST_ASSERT_TRUE(m.arena_full);
}

// ------------------------------------------------- ownership by LOCAL name (ticket 27)

static void test_owns_local_answers_by_the_card_name_not_the_share_path(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1234, 99));

    // The point of the function: find() keys on the SHARE path and cannot answer this.
    TEST_ASSERT_TRUE(smb_manifest_owns_local(&s_m, "photo1.jpg"));
    TEST_ASSERT_EQUAL_INT(-1, smb_manifest_find(&s_m, "photo1.jpg"));

    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "upload.jpg"));
}

static void test_owns_local_on_an_empty_manifest_and_on_nothing(void)
{
    // The state at boot before .smbidx is read, and the state on a card with no mirror. Both
    // must say "not owned" rather than trip: the count is taken on the on-demand path too, where
    // an unreadable manifest is a normal condition.
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "photo1.jpg"));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(NULL, "photo1.jpg"));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, NULL));
}

static void test_owns_local_is_not_a_prefix_match(void)
{
    // "photo1.jpg" must not own "photo1.jpg.thm" or "photo1.jpgx", and a shorter name must not
    // match a longer record. A prefix rule here would silently absorb the sidecar population and
    // the count would read as a healthy zero on a card full of orphans.
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1, 2));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "photo1.jpg.thm"));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "photo1.jpgx"));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "photo1"));
}

static void test_owns_local_is_case_sensitive_and_that_is_the_chosen_rule(void)
{
    // THE FIXTURE DISAGREES UNDER THE TWO CANDIDATE RULES, which is the only reason it is worth
    // writing: an exact compare says PHOTO1.JPG is not owned, a FAT-flavoured case-insensitive
    // one says it is.
    //
    // Exact is chosen. Both sides of the compare have one origin -- smb_manifest_local_name()
    // produced the record's name and the mirror wrote the file under that same string -- so a
    // mirrored file matches byte for byte and a loose compare buys nothing there. And because FAT
    // is case-insensitive, a file named PHOTO1.JPG CANNOT sit beside photo1.jpg on the card, so
    // the loose rule has no real collision to resolve either; all it could do is claim a file the
    // mirror never wrote and hide it from the count.
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1, 2));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "PHOTO1.JPG"));
    TEST_ASSERT_TRUE(smb_manifest_owns_local(&s_m, "photo1.jpg"));
}

static void test_owns_local_follows_a_removal(void)
{
    // evict_to_cache_limit() removes records, and the count is taken after it. A stale answer
    // here would show an evicted photograph as owned for one window and then jump.
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "a.jpg", "f/a.jpg", 1, 2));
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "b.jpg", "f/b.jpg", 3, 4));
    TEST_ASSERT_TRUE(smb_manifest_owns_local(&s_m, "a.jpg"));
    TEST_ASSERT_TRUE(smb_manifest_remove_at(&s_m, 0));
    TEST_ASSERT_FALSE(smb_manifest_owns_local(&s_m, "a.jpg"));
    TEST_ASSERT_TRUE(smb_manifest_owns_local(&s_m, "b.jpg"));
}

// ---------------------------------------------------------------- serialisation

static void test_round_trip(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1234, 99));
    TEST_ASSERT_TRUE(
        smb_manifest_add(&s_m, "smb_deadbeef.png", "20260511/a b c.png", 5, 6));

    char buf[512];
    const size_t n = smb_manifest_serialise(&s_m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    char arena2[1024];
    smb_manifest_t m2;
    TEST_ASSERT_TRUE(smb_manifest_init(&m2, arena2, sizeof(arena2)));
    TEST_ASSERT_TRUE(smb_manifest_parse(&m2, buf, n));

    TEST_ASSERT_EQUAL_UINT(2, (unsigned)smb_manifest_count(&m2));
    TEST_ASSERT_EQUAL_STRING("photo1.jpg", smb_manifest_local(&m2, 0));
    TEST_ASSERT_EQUAL_STRING("20260511/photo1.jpg", smb_manifest_path(&m2, 0));
    TEST_ASSERT_EQUAL_UINT64(1234, smb_manifest_size(&m2, 0));
    TEST_ASSERT_EQUAL_UINT64(99, smb_manifest_mtime(&m2, 0));
    // A space in a path survives: the separator is tab precisely so it can.
    TEST_ASSERT_EQUAL_STRING("20260511/a b c.png", smb_manifest_path(&m2, 1));
}

static void test_an_empty_manifest_round_trips(void)
{
    // Zero mirrored files is a success, not a failure -- conflating the two is the bug
    // ComittoNxA shipped, and board_smb_list() records the same rule.
    char buf[64];
    const size_t n = smb_manifest_serialise(&s_m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(smb_manifest_parse(&s_m, buf, n));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)smb_manifest_count(&s_m));
}

static void test_a_serialise_that_does_not_fit_writes_nothing(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "photo1.jpg", "20260511/photo1.jpg", 1234, 99));
    char buf[16];
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)smb_manifest_serialise(&s_m, buf, sizeof(buf)));
}

static void expect_parse_failure(const char *text)
{
    char arena2[1024];
    smb_manifest_t m2;
    smb_manifest_init(&m2, arena2, sizeof(arena2));
    TEST_ASSERT_FALSE(smb_manifest_parse(&m2, text, strlen(text)));
    // A failed parse leaves nothing behind: the recovery is a full re-fetch, and a
    // half-loaded index would make it a partial one.
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)smb_manifest_count(&m2));
}

static void test_malformed_manifests_are_rejected_wholesale(void)
{
    expect_parse_failure("");
    expect_parse_failure("#smbidx 2\n");                       // wrong version
    expect_parse_failure("photo1.jpg\t1\t2\tp.jpg\n");         // no header
    expect_parse_failure("#smbidx 1\nphoto1.jpg\t1\t2\n");     // missing field
    expect_parse_failure("#smbidx 1\nphoto1.jpg\tx\t2\tp\n");  // non-numeric size
    expect_parse_failure("#smbidx 1\nphoto1.jpg\t1\t2\tp");    // truncated, no newline
    expect_parse_failure("#smbidx 1\n\t1\t2\tp.jpg\n");        // empty local name
}

static void test_a_good_record_before_a_bad_one_is_still_discarded(void)
{
    expect_parse_failure("#smbidx 1\na.jpg\t1\t2\tp/a.jpg\nb.jpg\tNOPE\t2\tp/b.jpg\n");
}

// ------------------------------------------------- ticket 78: what a truncated index parses as
//
// The one case a truncated .smbidx can be that no log line reports: a cut landing exactly on a
// record boundary parses SUCCESSFULLY with a prefix of the records, which is indistinguishable
// from an index of a smaller cache. Every other cut fails the parse, which manifest_load() logs.
//
// This is a characterisation, not a proposal: the parser's behaviour is correct for a file that
// is whole, and the fix for the truncation is at the WRITER (ticket 78 section 3). The sweep is
// here because it is the only part of that ticket a host can reach, and because the two classes
// are the argument for fixing the writer at all -- so if the parser ever grows a length or a
// record count in its header, this test failing is the notice that the argument changed.
//
// Independently checked rather than asserted from the same logic that produces it: a short parse
// must yield exactly as many records as there are '\n' in the prefix after the header. That is
// what says the prefix is CLEAN. If take_field() ever accepted a field terminated by the end of
// the buffer instead of by its separator, a partial last line would parse too and the count
// would run one ahead of the newlines -- a corrupt record rather than a missing one.
static void test_a_truncated_index_parses_short_only_at_a_record_boundary(void)
{
    enum { RECORDS = 40 };
    for (int i = 0; i < RECORDS; i++) {
        char local[32], path[64];
        snprintf(local, sizeof(local), "IMG_%04d.jpg", i);
        snprintf(path, sizeof(path), "photos/202606/IMG_%04d.jpg", i);
        TEST_ASSERT_TRUE(smb_manifest_add(&s_m, local, path, 1048576u + (uint64_t)i, 1757000000u));
    }

    static char buf[8192];
    const size_t n = smb_manifest_serialise(&s_m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    static char arena2[8192];
    smb_manifest_t m2;

    unsigned failed = 0, short_parse = 0, complete = 0;
    size_t first_short = 0, last_short = 0;
    for (size_t cut = 0; cut <= n; cut++) {
        TEST_ASSERT_TRUE(smb_manifest_init(&m2, arena2, sizeof(arena2)));
        if (!smb_manifest_parse(&m2, buf, cut)) {
            failed++;
            continue;
        }
        const size_t got = smb_manifest_count(&m2);

        // The independent check. strlen(MANIFEST_HEADER) is 10 and is not exported; a parse that
        // succeeded proves the header was there, so counting newlines from byte 10 is sound.
        size_t newlines = 0;
        for (size_t i = 10; i < cut; i++) {
            if (buf[i] == '\n') {
                newlines++;
            }
        }
        TEST_ASSERT_EQUAL_UINT(newlines, got);

        if (got == RECORDS) {
            complete++;
        } else {
            short_parse++;
            if (first_short == 0) {
                first_short = cut;
            }
            last_short = cut;
        }
    }

    // Raw numbers, so the verdict is computed here in the open rather than inside the loop.
    printf("ticket 78 truncation sweep: %u bytes, %u cut points: %u failed, %u parsed short, "
           "%u parsed whole; short cuts %u..%u\n",
           (unsigned)n, (unsigned)n + 1, failed, short_parse, complete, (unsigned)first_short,
           (unsigned)last_short);

    // One cut per record boundary, counting the header-only file (which parses as "owning
    // nothing") and excluding the untruncated one.
    TEST_ASSERT_EQUAL_UINT(RECORDS, short_parse);
    TEST_ASSERT_EQUAL_UINT(1, complete);
    TEST_ASSERT_EQUAL_UINT((unsigned)n - RECORDS, failed);

    // The header-only prefix is the first of them, and it is the worst: it parses, so the mirror
    // is told it owns nothing at all rather than being told the file is damaged.
    TEST_ASSERT_EQUAL_UINT(10, (unsigned)first_short);
}

// ------------------------------------------------------------------- eviction

// The point of remove_at is the ORDER it leaves behind, because under the on-demand cache the
// record order is the fetch order and that is the whole eviction policy. A swap-with-last
// implementation passes a test that only counts records, so this asserts the sequence.
static void test_remove_at_preserves_the_order_of_the_rest(void)
{
    for (int i = 0; i < 5; i++) {
        char local[32], path[64];
        snprintf(local, sizeof(local), "f%d.jpg", i);
        snprintf(path, sizeof(path), "d/f%d.jpg", i);
        TEST_ASSERT_TRUE(smb_manifest_add(&s_m, local, path, (uint64_t)(i + 1), 100));
    }
    TEST_ASSERT_EQUAL_UINT(5, smb_manifest_count(&s_m));

    // The middle one, which is where a swap-with-last and a shift differ.
    TEST_ASSERT_TRUE(smb_manifest_remove_at(&s_m, 1));
    TEST_ASSERT_EQUAL_UINT(4, smb_manifest_count(&s_m));
    TEST_ASSERT_EQUAL_STRING("f0.jpg", smb_manifest_local(&s_m, 0));
    TEST_ASSERT_EQUAL_STRING("f2.jpg", smb_manifest_local(&s_m, 1));
    TEST_ASSERT_EQUAL_STRING("f3.jpg", smb_manifest_local(&s_m, 2));
    TEST_ASSERT_EQUAL_STRING("f4.jpg", smb_manifest_local(&s_m, 3));

    // The record travels whole, not just its name.
    TEST_ASSERT_EQUAL_STRING("d/f2.jpg", smb_manifest_path(&s_m, 1));
    TEST_ASSERT_EQUAL_UINT64(3, smb_manifest_size(&s_m, 1));

    // The first, then the last -- the two boundaries.
    TEST_ASSERT_TRUE(smb_manifest_remove_at(&s_m, 0));
    TEST_ASSERT_EQUAL_STRING("f2.jpg", smb_manifest_local(&s_m, 0));
    TEST_ASSERT_TRUE(smb_manifest_remove_at(&s_m, smb_manifest_count(&s_m) - 1));
    TEST_ASSERT_EQUAL_UINT(2, smb_manifest_count(&s_m));
    TEST_ASSERT_EQUAL_STRING("f2.jpg", smb_manifest_local(&s_m, 0));
    TEST_ASSERT_EQUAL_STRING("f3.jpg", smb_manifest_local(&s_m, 1));
}

static void test_remove_at_refuses_out_of_range(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "a.jpg", "d/a.jpg", 1, 2));
    TEST_ASSERT_FALSE(smb_manifest_remove_at(&s_m, 1));
    TEST_ASSERT_FALSE(smb_manifest_remove_at(&s_m, 99));
    TEST_ASSERT_FALSE(smb_manifest_remove_at(NULL, 0));
    TEST_ASSERT_EQUAL_UINT(1, smb_manifest_count(&s_m));
}

// A removal makes room, and the record that goes in afterwards must be findable -- which is
// what would break if count and the arena disagreed.
static void test_a_removal_makes_room_and_find_still_works(void)
{
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "a.jpg", "d/a.jpg", 1, 2));
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "b.jpg", "d/b.jpg", 3, 4));
    TEST_ASSERT_TRUE(smb_manifest_remove_at(&s_m, 0));
    TEST_ASSERT_EQUAL_INT(-1, smb_manifest_find(&s_m, "d/a.jpg"));
    TEST_ASSERT_EQUAL_INT(0, smb_manifest_find(&s_m, "d/b.jpg"));
    TEST_ASSERT_TRUE(smb_manifest_add(&s_m, "c.jpg", "d/c.jpg", 5, 6));
    TEST_ASSERT_EQUAL_INT(1, smb_manifest_find(&s_m, "d/c.jpg"));
    TEST_ASSERT_EQUAL_STRING("d/b.jpg", smb_manifest_path(&s_m, 0));
}


// ------------------------------------------------------- the on-demand cache's floor

#define MB (1024ull * 1024ull)

// The two volumes this frame actually has, named rather than computed, so a change to the
// formula has to be argued against the real hardware and not against round numbers.
static void test_reserve_at_the_two_real_volumes(void)
{
    // partitions.csv:25 -- the 6 MB internal partition, which is what /data is with no card.
    TEST_ASSERT_EQUAL_UINT64(1 * MB, smb_cache_reserve_bytes(6ull * MB));
    // The fitted card, GET /api/storage 2026-09-09.
    TEST_ASSERT_EQUAL_UINT64(32 * MB, smb_cache_reserve_bytes(1985802240ull));
}

// The clamps, and one value either side of each, so a formula that ignored the division
// entirely would still fail this.
static void test_reserve_clamps_both_ends(void)
{
    TEST_ASSERT_EQUAL_UINT64(1 * MB, smb_cache_reserve_bytes(0));
    TEST_ASSERT_EQUAL_UINT64(1 * MB, smb_cache_reserve_bytes(16 * MB - 16));
    TEST_ASSERT_EQUAL_UINT64(1 * MB, smb_cache_reserve_bytes(16 * MB));
    TEST_ASSERT_EQUAL_UINT64(1 * MB + 1, smb_cache_reserve_bytes(16 * MB + 16));

    TEST_ASSERT_EQUAL_UINT64(32 * MB - 1, smb_cache_reserve_bytes(512 * MB - 16));
    TEST_ASSERT_EQUAL_UINT64(32 * MB, smb_cache_reserve_bytes(512 * MB));
    TEST_ASSERT_EQUAL_UINT64(32 * MB, smb_cache_reserve_bytes(512 * MB + 16));
    TEST_ASSERT_EQUAL_UINT64(32 * MB, smb_cache_reserve_bytes(64ull * 1024 * MB));
}

// In the band between the clamps it really is a sixteenth, and it is monotone. A clamp
// applied in the wrong order, or a low clamp that swallowed the band, passes the two tests
// above on the internal volume and fails here.
static void test_reserve_is_a_sixteenth_between_the_clamps(void)
{
    TEST_ASSERT_EQUAL_UINT64(6 * MB, smb_cache_reserve_bytes(96 * MB));
    TEST_ASSERT_EQUAL_UINT64(16 * MB, smb_cache_reserve_bytes(256 * MB));
    uint64_t prev = 0;
    for (uint64_t t = 16 * MB; t <= 512 * MB; t += 8 * MB) {
        const uint64_t r = smb_cache_reserve_bytes(t);
        TEST_ASSERT_TRUE(r >= prev);
        TEST_ASSERT_TRUE(r >= 1 * MB && r <= 32 * MB);
        prev = r;
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ascii_leaf_is_kept_verbatim);
    RUN_TEST(test_case_is_preserved_on_a_verbatim_name);
    RUN_TEST(test_all_four_extensions_are_accepted);
    RUN_TEST(test_non_image_extensions_are_rejected);
    RUN_TEST(test_a_dotfile_is_not_a_photograph);
    RUN_TEST(test_a_leading_dot_is_rejected_however_late_the_extension);
    RUN_TEST(test_non_ascii_leaf_becomes_a_hash);
    RUN_TEST(test_a_hostile_character_becomes_a_hash);
    RUN_TEST(test_an_overlong_ascii_name_becomes_a_hash);
    RUN_TEST(test_the_hash_covers_the_path_not_the_leaf);
    RUN_TEST(test_the_hashed_extension_is_lowercased);
    RUN_TEST(test_add_find_and_change_detection);
    RUN_TEST(test_a_full_arena_rejects_cleanly);
    RUN_TEST(test_owns_local_answers_by_the_card_name_not_the_share_path);
    RUN_TEST(test_owns_local_on_an_empty_manifest_and_on_nothing);
    RUN_TEST(test_owns_local_is_not_a_prefix_match);
    RUN_TEST(test_owns_local_is_case_sensitive_and_that_is_the_chosen_rule);
    RUN_TEST(test_owns_local_follows_a_removal);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_an_empty_manifest_round_trips);
    RUN_TEST(test_a_serialise_that_does_not_fit_writes_nothing);
    RUN_TEST(test_malformed_manifests_are_rejected_wholesale);
    RUN_TEST(test_a_good_record_before_a_bad_one_is_still_discarded);
    RUN_TEST(test_a_truncated_index_parses_short_only_at_a_record_boundary);
    RUN_TEST(test_remove_at_preserves_the_order_of_the_rest);
    RUN_TEST(test_remove_at_refuses_out_of_range);
    RUN_TEST(test_a_removal_makes_room_and_find_still_works);

    RUN_TEST(test_reserve_at_the_two_real_volumes);
    RUN_TEST(test_reserve_clamps_both_ends);
    RUN_TEST(test_reserve_is_a_sixteenth_between_the_clamps);
    return UNITY_END();
}
