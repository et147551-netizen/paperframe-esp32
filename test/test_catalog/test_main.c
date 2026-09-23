// Ticket 37: the share catalogue and the shuffle epoch, on the host.
//
// The two things worth stating about what is pinned here. The PERMUTATION tests assert
// bijectivity rather than a fixed sequence, because a fixed sequence would pin xorshift32's
// output and break for any PRNG change while telling us nothing about the property that
// matters -- every photograph exactly once per pass. The one place a fixed sequence IS pinned
// is reproducibility from a seed, which is the property the six bytes of persisted state rest
// on.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smb_catalog.h"
#include "unity.h"

// 4096 paths of up to 400 bytes would be 1.6 MB, which is a PSRAM figure and not a stack one.
// The tests never fill the catalogue, so the arena is sized for what they do use.
static char s_arena[64 * 1024];
static smb_catalog_t s_cat;

void setUp(void)
{
    TEST_ASSERT_TRUE(smb_catalog_init(&s_cat, s_arena, sizeof(s_arena)));
}

void tearDown(void) {}

// --------------------------------------------------------------------------- add / find

static void test_add_and_read_back(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1234, 99));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/b.jpg", 5678, 100));
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("202606/a.jpg", smb_catalog_path(&s_cat, 0));
    TEST_ASSERT_EQUAL_UINT64(1234, smb_catalog_size(&s_cat, 0));
    TEST_ASSERT_EQUAL_UINT64(100, smb_catalog_mtime(&s_cat, 1));
    TEST_ASSERT_EQUAL_INT(1, smb_catalog_find(&s_cat, "202606/b.jpg"));
    TEST_ASSERT_EQUAL_INT(-1, smb_catalog_find(&s_cat, "202606/c.jpg"));
}

// A folder listed twice without a drop must not weight its photographs double in the shuffle.
static void test_a_duplicate_path_is_refused(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    TEST_ASSERT_FALSE(smb_catalog_add(&s_cat, "202606/a.jpg", 999, 999));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    // The first record is untouched, not overwritten by the refused one.
    TEST_ASSERT_EQUAL_UINT64(1, smb_catalog_size(&s_cat, 0));
}

static void test_a_bad_path_is_refused_cleanly(void)
{
    char toolong[SMB_CATALOG_PATH_SIZE + 8];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';

    TEST_ASSERT_FALSE(smb_catalog_add(&s_cat, "", 1, 1));
    TEST_ASSERT_FALSE(smb_catalog_add(&s_cat, NULL, 1, 1));
    TEST_ASSERT_FALSE(smb_catalog_add(&s_cat, toolong, 1, 1));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_count(&s_cat));
    TEST_ASSERT_FALSE(s_cat.arena_full);
}

static void test_a_full_arena_says_so_and_stays_usable(void)
{
    char small[64];
    smb_catalog_t c;
    TEST_ASSERT_TRUE(smb_catalog_init(&c, small, sizeof(small)));
    // "202606/aNNN.jpg" is 15 bytes plus a terminator, so four fit in 64 and the fifth
    // does not.
    int added = 0;
    for (int i = 0; i < 8; i++) {
        char p[32];
        snprintf(p, sizeof(p), "202606/a%03d.jpg", i);
        if (smb_catalog_add(&c, p, 1, 1)) {
            added++;
        }
    }
    TEST_ASSERT_EQUAL_INT(4, added);
    TEST_ASSERT_TRUE(c.arena_full);
    TEST_ASSERT_EQUAL_UINT32(4, smb_catalog_count(&c));
    // Not corrupted by the refusal: every kept record still reads back.
    TEST_ASSERT_EQUAL_STRING("202606/a000.jpg", smb_catalog_path(&c, 0));
    TEST_ASSERT_EQUAL_STRING("202606/a003.jpg", smb_catalog_path(&c, 3));
}

// -------------------------------------------------------------------------- drop_folder

// THE DISCRIMINATING DIRECTION IS DROPPING THE SHORTER NAME, and the first version of this
// test did not test what it is named after. It dropped "202606" and asserted "20260511"
// survived -- but those two differ at byte 5, so a strncmp(own, folder, strlen(folder)) prefix
// test distinguishes them perfectly and passed the test unchanged. Verified by inverting the
// implementation on purpose: the prefix version passed this case and was caught only by
// test_drop_folder_handles_the_root, by accident. The assumption was living in the fixture.
//
// A prefix test is wrong exactly when one folder name is a STRICT PREFIX of another and the
// short one is dropped: "2026" against "202606". Both directions are asserted here now.
static void test_drop_folder_is_exact_not_a_prefix(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "2026/short.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/b.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "20260511/c.jpg", 1, 1));

    // Dropping the SHORT name must take exactly its own one record.
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_drop_folder(&s_cat, "2026", NULL));
    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_count(&s_cat));
    TEST_ASSERT_TRUE(smb_catalog_find(&s_cat, "202606/a.jpg") >= 0);
    TEST_ASSERT_TRUE(smb_catalog_find(&s_cat, "202606/b.jpg") >= 0);
    TEST_ASSERT_TRUE(smb_catalog_find(&s_cat, "20260511/c.jpg") >= 0);

    // And the long name takes its two and leaves the other folder alone.
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_drop_folder(&s_cat, "202606", NULL));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("20260511/c.jpg", smb_catalog_path(&s_cat, 0));
}

static void test_drop_folder_handles_the_root(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "loose.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_drop_folder(&s_cat, "", NULL));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("202606/a.jpg", smb_catalog_path(&s_cat, 0));
}

// ------------------------------------------------------- a folder that has left smb_path
//
// Ticket 51 found the hole: drop_folder() only ever runs for a folder the round-robin still
// lists, so records of a folder REMOVED from smb_path stay for good and are then selected,
// fail to fetch, and cost EPOCH_MISS_MAX advances each.

static void test_keep_folders_drops_a_folder_that_left_the_list(void)
{
    static const char *const paths[] = {
        "a/1.jpg", "a/2.jpg", "gone/x.jpg", "b/1.jpg", "gone/y.jpg", "b/2.jpg",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
        TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, paths[i], 1, 1));
    }
    s_cat.truncated = true;

    // A human-typed list, because that is what the setting holds.
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_keep_folders(&s_cat, " a , b "));

    // Index for index, not a count and not a sample: the survivors must keep their order, or
    // the epoch permutation and the want list quietly come to mean something else -- the same
    // failure tickets 44 and 45 fixed for the refresh path.
    static const char *const left[] = {"a/1.jpg", "a/2.jpg", "b/1.jpg", "b/2.jpg"};
    TEST_ASSERT_EQUAL_UINT32(4, smb_catalog_count(&s_cat));
    for (size_t i = 0; i < sizeof(left) / sizeof(*left); i++) {
        TEST_ASSERT_EQUAL_STRING(left[i], smb_catalog_path(&s_cat, i));
    }
    // Same reasoning as drop_folder: the flag described a count that no longer holds.
    TEST_ASSERT_FALSE(s_cat.truncated);
}

static void test_keep_folders_is_exact_not_a_prefix(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "2026/short.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "20260511/c.jpg", 1, 1));

    // THE DISCRIMINATING DIRECTION IS THE SHORT NAME, and the first version of this test had
    // only the long one -- which a prefix implementation passes, because "20260511" and "202606"
    // diverge inside the shorter name's own length. Found by perturbing strcmp to strncmp and
    // watching all four of these tests still pass. Listing "2026" must keep its one record and
    // drop both folders whose names begin with it.
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_keep_folders(&s_cat, "2026"));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("2026/short.jpg", smb_catalog_path(&s_cat, 0));
}

// And the other direction, which is the one this bench's own share has: a list naming the LONG
// folder must not keep the short one.
static void test_keep_folders_does_not_keep_a_shorter_folder(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "2026/short.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_keep_folders(&s_cat, "202606"));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("202606/a.jpg", smb_catalog_path(&s_cat, 0));
}

// The deliberate exception, pinned so nobody "fixes" it: an empty smb_path means THE SHARE
// ROOT, which a list cannot name, and catalogue_next_folder() does nothing at all in that case.
// Dropping everything on a momentarily blank setting would cost a full re-listing per folder.
static void test_keep_folders_with_an_empty_list_drops_nothing(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "a/1.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/1.jpg", 1, 1));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_keep_folders(&s_cat, ""));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_keep_folders(&s_cat, " , "));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_keep_folders(&s_cat, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_count(&s_cat));
}

// A record at the share root has folder "", and no element of a non-empty list ever trims to
// "", so it is a stray by construction and goes.
static void test_keep_folders_drops_a_root_record_against_a_real_list(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "loose.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "a/1.jpg", 1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_keep_folders(&s_cat, "a"));
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_STRING("a/1.jpg", smb_catalog_path(&s_cat, 0));
}

// A REFRESH OF AN UNCHANGED FOLDER MUST NOT MOVE ANY INDEX. Tickets 44 and 45: drop_folder()
// compacts and smb_catalog_add() appends, so the refreshed folder used to land at the tail and
// renumber every record after its old position -- 120 to 230 of them per hourly refresh on the
// bench share, with nothing on the share changed. Three things hold indices across a refresh
// (the epoch permutation, the want list, the fetch counter) and all three then meant something
// else. The whole catalogue is compared index for index, not a sample, because the failure was
// silent and a sample is how it stayed silent.
static void test_refreshing_an_unchanged_folder_moves_no_index(void)
{
    static const char *const paths[] = {
        "a/1.jpg", "a/2.jpg", "b/1.jpg", "b/2.jpg", "b/3.jpg", "c/1.jpg", "c/2.jpg",
    };
    const size_t n = sizeof(paths) / sizeof(paths[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, paths[i], (uint64_t)(i + 1), 100 + i));
    }

    // Refresh the MIDDLE folder, which is the case that renumbers: "a" would rotate by zero.
    size_t first = 0;
    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_drop_folder(&s_cat, "b", &first));
    TEST_ASSERT_EQUAL_UINT32(2, first);
    const size_t tail = smb_catalog_count(&s_cat);
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/1.jpg", 3, 102));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/2.jpg", 4, 103));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/3.jpg", 5, 104));
    smb_catalog_restore_at(&s_cat, tail, first);

    TEST_ASSERT_EQUAL_UINT32(n, smb_catalog_count(&s_cat));
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(paths[i], smb_catalog_path(&s_cat, i));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(i + 1), smb_catalog_size(&s_cat, i));
    }

    // And doing it again is still stable -- a rotate that is right once and drifts is worse
    // than one that is wrong, because only the second refresh would show it.
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_drop_folder(&s_cat, "c", &first));
    TEST_ASSERT_EQUAL_UINT32(5, first);
    const size_t tail2 = smb_catalog_count(&s_cat);
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "c/1.jpg", 6, 105));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "c/2.jpg", 7, 106));
    smb_catalog_restore_at(&s_cat, tail2, first);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(paths[i], smb_catalog_path(&s_cat, i));
    }
}

// A folder that GAINED and LOST files still lands in its own place, and only its own region
// moves -- the records before it must not shift at all, and the ones after it shift by the
// folder's own delta and nothing more.
static void test_a_changed_folder_shifts_only_what_follows_it(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "a/1.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/1.jpg", 2, 2));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/2.jpg", 3, 3));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "c/1.jpg", 4, 4));

    size_t first = 0;
    TEST_ASSERT_EQUAL_UINT32(2, smb_catalog_drop_folder(&s_cat, "b", &first));
    const size_t tail = smb_catalog_count(&s_cat);
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/2.jpg", 3, 3)); // 1.jpg gone, 9.jpg new
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "b/9.jpg", 9, 9));
    smb_catalog_restore_at(&s_cat, tail, first);

    TEST_ASSERT_EQUAL_STRING("a/1.jpg", smb_catalog_path(&s_cat, 0)); // before: unmoved
    TEST_ASSERT_EQUAL_STRING("b/2.jpg", smb_catalog_path(&s_cat, 1));
    TEST_ASSERT_EQUAL_STRING("b/9.jpg", smb_catalog_path(&s_cat, 2));
    TEST_ASSERT_EQUAL_STRING("c/1.jpg", smb_catalog_path(&s_cat, 3)); // after: same index here
}

// A folder that is NEW to the catalogue has nothing to restore to, and must simply land at the
// end without disturbing anything. `first` is the count in that case, so the rotate is a no-op.
//
// THIS ONE IS A GUARD, NOT A DISCRIMINATOR, and the difference is worth stating: it passes with
// the rotate removed entirely, because appending is what both versions do here. What it catches
// is the rotate MISFIRING on the no-op case. The two tests above are the ones that fail without
// the fix -- checked by neutering smb_catalog_restore_at() and watching them go red.
static void test_a_new_folder_appends(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "a/1.jpg", 1, 1));
    size_t first = 0;
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_drop_folder(&s_cat, "z", &first));
    TEST_ASSERT_EQUAL_UINT32(1, first);
    const size_t tail = smb_catalog_count(&s_cat);
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "z/1.jpg", 5, 5));
    smb_catalog_restore_at(&s_cat, tail, first);
    TEST_ASSERT_EQUAL_STRING("a/1.jpg", smb_catalog_path(&s_cat, 0));
    TEST_ASSERT_EQUAL_STRING("z/1.jpg", smb_catalog_path(&s_cat, 1));
}

// Re-cataloguing a folder replaces it. This is the whole point of drop_folder, so it gets the
// round trip rather than the two halves separately.
static void test_recataloguing_a_folder_replaces_it(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/gone.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/stays.jpg", 1, 1));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "250416/other.jpg", 1, 1));

    smb_catalog_drop_folder(&s_cat, "202606", NULL);
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/stays.jpg", 2, 2));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/new.jpg", 3, 3));

    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_count(&s_cat));
    TEST_ASSERT_EQUAL_INT(-1, smb_catalog_find(&s_cat, "202606/gone.jpg"));
    TEST_ASSERT_TRUE(smb_catalog_find(&s_cat, "202606/new.jpg") >= 0);
    TEST_ASSERT_TRUE(smb_catalog_find(&s_cat, "250416/other.jpg") >= 0);
    // The re-added record carries the NEW size, so a folder whose files changed is updated
    // rather than kept stale.
    TEST_ASSERT_EQUAL_UINT64(2, smb_catalog_size(&s_cat, smb_catalog_find(&s_cat, "202606/stays.jpg")));
}

// ------------------------------------------------------------------- serialise / parse

static void test_round_trip(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1234, 1700000000));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "loose.jpg", 0, 0));
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "20260511/\xE6\x97\xA5.jpg", 99, 5));

    char buf[4096];
    const size_t n = smb_catalog_serialise(&s_cat, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    smb_catalog_t back;
    static char back_arena[4096];
    TEST_ASSERT_TRUE(smb_catalog_init(&back, back_arena, sizeof(back_arena)));
    TEST_ASSERT_TRUE(smb_catalog_parse(&back, buf, n));

    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_count(&back));
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_STRING(smb_catalog_path(&s_cat, i), smb_catalog_path(&back, i));
        TEST_ASSERT_EQUAL_UINT64(smb_catalog_size(&s_cat, i), smb_catalog_size(&back, i));
        TEST_ASSERT_EQUAL_UINT64(smb_catalog_mtime(&s_cat, i), smb_catalog_mtime(&back, i));
    }
}

static void test_an_empty_catalogue_round_trips(void)
{
    char buf[64];
    const size_t n = smb_catalog_serialise(&s_cat, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    smb_catalog_t back;
    static char back_arena[64];
    TEST_ASSERT_TRUE(smb_catalog_init(&back, back_arena, sizeof(back_arena)));
    TEST_ASSERT_TRUE(smb_catalog_parse(&back, buf, n));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_count(&back));
}

static void test_a_serialise_that_does_not_fit_writes_nothing(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 1));
    char buf[12]; // room for the header and nothing else
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_serialise(&s_cat, buf, sizeof(buf)));
}

static void test_malformed_catalogues_are_rejected_wholesale(void)
{
    // Every literal carries the CURRENT header, so each case fails for the reason it names.
    // They were written against "#smbdir 1" and, when the format moved to 2, they would all
    // have kept passing on the version check alone -- testing nothing while looking green.
    static const char *const bad[] = {
        "1\t1\ta.jpg\n",                           // no header
        "#smbdir 2 0 0\n1\t1\n",                   // missing the path field
        "#smbdir 2 0 0\n1\tx\ta.jpg\n",            // non-numeric mtime
        "#smbdir 2 0 0\nx\t1\ta.jpg\n",            // non-numeric size
        "#smbdir 2 0 0\n1\t1\ta.jpg",              // unterminated last line
        "#smbdir 2 0 0\n1\t1\ta.jpg\n1\t1\ta.jpg\n", // a duplicate path
        "#smbdir 2 0 0\n1\t1\t\n",                 // empty path
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        smb_catalog_t c;
        static char arena[512];
        TEST_ASSERT_TRUE(smb_catalog_init(&c, arena, sizeof(arena)));
        TEST_ASSERT_FALSE_MESSAGE(smb_catalog_parse(&c, bad[i], strlen(bad[i])), bad[i]);
        TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_count(&c));
    }
}

// A good record before a bad one must not survive: the whole file is discarded, so there is
// one recovery path rather than a repair routine nobody exercises.
static void test_a_good_record_before_a_bad_one_is_still_discarded(void)
{
    static const char text[] = "#smbdir 2 0 0\n1\t1\tgood.jpg\nx\t1\tbad.jpg\n";
    TEST_ASSERT_FALSE(smb_catalog_parse(&s_cat, text, sizeof(text) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_count(&s_cat));
}

// ------------------------------------------------------------------- the shuffle epoch

static void assert_is_a_permutation(size_t n, uint32_t seed)
{
    uint16_t *perm = calloc(n ? n : 1, sizeof(uint16_t));
    unsigned char *seen = calloc(n ? n : 1, 1);
    TEST_ASSERT_NOT_NULL(perm);
    TEST_ASSERT_NOT_NULL(seen);

    smb_catalog_shuffle(n, seed, perm);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(perm[i] < n, "index out of range");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, seen[perm[i]], "index visited twice");
        seen[perm[i]] = 1;
    }
    free(perm);
    free(seen);
}

// EVERY photograph exactly once per pass, which is the property the whole selection scheme
// rests on -- not a fixed sequence, which would only pin xorshift32.
static void test_the_shuffle_is_a_bijection(void)
{
    // 230 and 500 are the slideshow's sizes rather than the catalogue's: app_slideshow.c walks
    // this same permutation over the photo list, whose cap is PHOTO_LIST_MAX = 500, and the card
    // held 230 photographs when that was written.
    const size_t sizes[] = {1, 2, 3, 200, 230, 243, 500, 1032, 1069, 2101, SMB_CATALOG_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        assert_is_a_permutation(sizes[i], 0x12345678u);
    }
}

static void test_a_zero_seed_still_shuffles(void)
{
    // xorshift32 fed zero produces zero forever, which would leave the identity permutation
    // and the same order on every frame that failed to read its stored seed.
    uint16_t a[64], b[64];
    smb_catalog_shuffle(64, 0, a);
    assert_is_a_permutation(64, 0);

    bool identity = true;
    for (size_t i = 0; i < 64; i++) {
        if (a[i] != (uint16_t)i) {
            identity = false;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(identity, "a zero seed left the identity permutation");

    // And it is still deterministic, so a frame that always reads zero is at least consistent.
    smb_catalog_shuffle(64, 0, b);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(a, b, 64);
}

// The property the six bytes of persisted state rest on: the permutation is recoverable from
// the seed alone, so a cursor stored beside it lands on the same photograph after a power cut.
static void test_the_shuffle_is_reproducible_from_the_seed(void)
{
    uint16_t a[512], b[512];
    smb_catalog_shuffle(512, 0xDEADBEEFu, a);
    smb_catalog_shuffle(512, 0xDEADBEEFu, b);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(a, b, 512);

    smb_catalog_shuffle(512, 0xDEADBEEEu, b);
    bool same = true;
    for (size_t i = 0; i < 512; i++) {
        if (a[i] != b[i]) {
            same = false;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(same, "two different seeds gave the same permutation");
}

// A single entry has one permutation and the loop must not underflow reaching it -- the
// downward Fisher-Yates starts at n-1, so n == 1 and n == 0 are the boundary.
static void test_the_degenerate_sizes_do_not_underflow(void)
{
    uint16_t one[1] = {0xFFFF};
    smb_catalog_shuffle(1, 42, one);
    TEST_ASSERT_EQUAL_UINT16(0, one[0]);

    uint16_t none[1] = {0xBEEF};
    smb_catalog_shuffle(0, 42, none);
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, none[0]); // untouched
}

// ------------------------------------------------------- the header's persisted state

// The folder cursor has to survive a reboot or a frame that restarts often never advances past
// folder 0 -- observed for real on 2026-09-06, twice, once in a test and once when the operator
// rebooted the board. It rides in the catalogue's own header because that file is already
// rewritten once per run, so persisting it costs nothing.
static void test_the_header_carries_the_cursor_and_seed(void)
{
    TEST_ASSERT_TRUE(smb_catalog_add(&s_cat, "202606/a.jpg", 1, 2));
    s_cat.folder_cursor = 4;
    s_cat.seed = 0xDEADBEEFu;

    char buf[1024];
    const size_t n = smb_catalog_serialise(&s_cat, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    smb_catalog_t back;
    static char back_arena[1024];
    TEST_ASSERT_TRUE(smb_catalog_init(&back, back_arena, sizeof(back_arena)));
    TEST_ASSERT_TRUE(smb_catalog_parse(&back, buf, n));
    TEST_ASSERT_EQUAL_UINT32(4, back.folder_cursor);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, back.seed);
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_count(&back));
}

// smb_catalog_init/reset must not leave the state at whatever the caller's memory held: an
// uninitialised cursor would index a folder that does not exist.
static void test_reset_clears_the_persisted_state(void)
{
    s_cat.folder_cursor = 9;
    s_cat.seed = 7;
    smb_catalog_reset(&s_cat);
    TEST_ASSERT_EQUAL_UINT32(0, s_cat.folder_cursor);
    TEST_ASSERT_EQUAL_UINT32(0, s_cat.seed);
}

// Version 1 had no cursor and no seed. It is rejected rather than migrated, and that is a
// decision rather than an oversight: the recovery is a re-listing over the next few runs, which
// is cheap, and it keeps ONE code path for a header this code does not recognise. The test
// exists so that the choice is visible instead of looking like a parser bug.
static void test_the_previous_format_is_rejected_not_migrated(void)
{
    static const char v1[] = "#smbdir 1\n1\t2\t202606/a.jpg\n";
    TEST_ASSERT_FALSE(smb_catalog_parse(&s_cat, v1, sizeof(v1) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_count(&s_cat));
}

static void test_a_malformed_header_is_rejected(void)
{
    static const char *const bad[] = {
        "#smbdir 2\n1\t2\ta.jpg\n",            // header without the two numbers
        "#smbdir 2 4\n1\t2\ta.jpg\n",          // only one number
        "#smbdir 2 x 1\n1\t2\ta.jpg\n",        // non-numeric cursor
        "#smbdir 2 4 y\n1\t2\ta.jpg\n",        // non-numeric seed
        "#smbdir 3 4 1\n1\t2\ta.jpg\n",        // a version from the future
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        smb_catalog_t c;
        static char arena[256];
        TEST_ASSERT_TRUE(smb_catalog_init(&c, arena, sizeof(arena)));
        TEST_ASSERT_FALSE_MESSAGE(smb_catalog_parse(&c, bad[i], strlen(bad[i])), bad[i]);
    }
}

// -------------------------------------------------------------------- the folder list

static void test_folder_list_counts_and_splits(void)
{
    char out[64];
    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_folder_count("202606,250416,20260511"));
    TEST_ASSERT_TRUE(smb_catalog_folder_at("202606,250416,20260511", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("250416", out);
    TEST_ASSERT_FALSE(smb_catalog_folder_at("202606,250416", 2, out, sizeof(out)));
}

// A single folder is a one-item list, which is what keeps every already-configured device
// working without touching its NVS.
static void test_a_single_folder_is_a_one_item_list(void)
{
    char out[64];
    TEST_ASSERT_EQUAL_UINT32(1, smb_catalog_folder_count("202606"));
    TEST_ASSERT_TRUE(smb_catalog_folder_at("202606", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("202606", out);
}

// An empty smb_path has always meant the share root, so it must count as zero folders rather
// than as one folder named "".
static void test_an_empty_list_is_no_folders(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_folder_count(""));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_folder_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_folder_count(",,  ,"));
    TEST_ASSERT_EQUAL_UINT32(0, smb_catalog_folder_count("/"));
}

// What a person actually types into a form.
static void test_the_list_survives_human_input(void)
{
    char out[64];
    const char *messy = " 202606 , /250416/ ,, 20260511,";
    TEST_ASSERT_EQUAL_UINT32(3, smb_catalog_folder_count(messy));
    TEST_ASSERT_TRUE(smb_catalog_folder_at(messy, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("202606", out);
    // The slashes come off both ends, so appending "/leaf.jpg" cannot double a separator.
    TEST_ASSERT_TRUE(smb_catalog_folder_at(messy, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("250416", out);
    TEST_ASSERT_TRUE(smb_catalog_folder_at(messy, 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("20260511", out);
}

// An inner separator is a nested folder and must survive, or "a/b" becomes unreachable.
static void test_a_nested_folder_keeps_its_inner_slash(void)
{
    char out[64];
    TEST_ASSERT_TRUE(smb_catalog_folder_at("/photos/2026/", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("photos/2026", out);
}

static void test_a_folder_too_long_for_the_buffer_is_refused(void)
{
    char out[8];
    TEST_ASSERT_FALSE(smb_catalog_folder_at("a-very-long-folder-name", 0, out, sizeof(out)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_add_and_read_back);
    RUN_TEST(test_a_duplicate_path_is_refused);
    RUN_TEST(test_a_bad_path_is_refused_cleanly);
    RUN_TEST(test_a_full_arena_says_so_and_stays_usable);
    RUN_TEST(test_drop_folder_is_exact_not_a_prefix);
    RUN_TEST(test_drop_folder_handles_the_root);
    RUN_TEST(test_keep_folders_drops_a_folder_that_left_the_list);
    RUN_TEST(test_keep_folders_is_exact_not_a_prefix);
    RUN_TEST(test_keep_folders_does_not_keep_a_shorter_folder);
    RUN_TEST(test_keep_folders_with_an_empty_list_drops_nothing);
    RUN_TEST(test_keep_folders_drops_a_root_record_against_a_real_list);
    RUN_TEST(test_recataloguing_a_folder_replaces_it);
    RUN_TEST(test_refreshing_an_unchanged_folder_moves_no_index);
    RUN_TEST(test_a_changed_folder_shifts_only_what_follows_it);
    RUN_TEST(test_a_new_folder_appends);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_an_empty_catalogue_round_trips);
    RUN_TEST(test_a_serialise_that_does_not_fit_writes_nothing);
    RUN_TEST(test_malformed_catalogues_are_rejected_wholesale);
    RUN_TEST(test_a_good_record_before_a_bad_one_is_still_discarded);
    RUN_TEST(test_the_shuffle_is_a_bijection);
    RUN_TEST(test_a_zero_seed_still_shuffles);
    RUN_TEST(test_the_shuffle_is_reproducible_from_the_seed);
    RUN_TEST(test_the_degenerate_sizes_do_not_underflow);
    RUN_TEST(test_the_header_carries_the_cursor_and_seed);
    RUN_TEST(test_reset_clears_the_persisted_state);
    RUN_TEST(test_the_previous_format_is_rejected_not_migrated);
    RUN_TEST(test_a_malformed_header_is_rejected);
    RUN_TEST(test_folder_list_counts_and_splits);
    RUN_TEST(test_a_single_folder_is_a_one_item_list);
    RUN_TEST(test_an_empty_list_is_no_folders);
    RUN_TEST(test_the_list_survives_human_input);
    RUN_TEST(test_a_nested_folder_keeps_its_inner_slash);
    RUN_TEST(test_a_folder_too_long_for_the_buffer_is_refused);
    return UNITY_END();
}
