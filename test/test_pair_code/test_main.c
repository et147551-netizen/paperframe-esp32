// Host-side tests for the pairing code's canonical form.
//
// The mapping being tested is the one a *reader* needs: a code read off a dithered E Ink panel
// and typed back in, where O for 0 and l for 1 are the mistakes that actually happen. The
// randomness and the five-attempt window are src/app/app_auth.c's and are not testable here.

#include <string.h>

#include <unity.h>

#include "pair_code.h"

void setUp(void) {}
void tearDown(void) {}

static char out[PAIR_CODE_SIZE];

// ------------------------------------------------------------------- from bytes

static void test_five_bytes_become_eight_symbols(void)
{
    const uint8_t zero[PAIR_CODE_BYTES] = {0, 0, 0, 0, 0};
    TEST_ASSERT_TRUE(pair_code_from_bytes(zero, sizeof(zero), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("00000000", out);

    const uint8_t all[PAIR_CODE_BYTES] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(pair_code_from_bytes(all, sizeof(all), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ZZZZZZZZ", out);
}

// 40 bits, most significant symbol first. 0x01 in the first byte is bit 32, which lands in the
// second symbol (bits 34-30), not the first.
static void test_the_bit_order_is_most_significant_first(void)
{
    const uint8_t b[PAIR_CODE_BYTES] = {0x00, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_TRUE(pair_code_from_bytes(b, sizeof(b), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("00000001", out);

    const uint8_t c[PAIR_CODE_BYTES] = {0xF8, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_TRUE(pair_code_from_bytes(c, sizeof(c), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Z0000000", out);
}

// Every symbol produced must be in the alphabet, which is the property that keeps I, L, O and U
// off the glass. Walked over all 32 values by putting each in the low five bits.
static void test_every_symbol_is_in_the_alphabet(void)
{
    for (unsigned v = 0; v < 32; v++) {
        const uint8_t b[PAIR_CODE_BYTES] = {0, 0, 0, 0, (uint8_t)v};
        TEST_ASSERT_TRUE(pair_code_from_bytes(b, sizeof(b), out, sizeof(out)));
        TEST_ASSERT_NOT_NULL(strchr(PAIR_CODE_ALPHABET, out[PAIR_CODE_LEN - 1]));
        TEST_ASSERT_EQUAL_CHAR(PAIR_CODE_ALPHABET[v], out[PAIR_CODE_LEN - 1]);
    }
    TEST_ASSERT_NULL(strchr(PAIR_CODE_ALPHABET, 'I'));
    TEST_ASSERT_NULL(strchr(PAIR_CODE_ALPHABET, 'L'));
    TEST_ASSERT_NULL(strchr(PAIR_CODE_ALPHABET, 'O'));
    TEST_ASSERT_NULL(strchr(PAIR_CODE_ALPHABET, 'U'));
    TEST_ASSERT_EQUAL_UINT32(32u, (uint32_t)strlen(PAIR_CODE_ALPHABET));
}

static void test_from_bytes_refuses_the_wrong_shape(void)
{
    const uint8_t b[PAIR_CODE_BYTES] = {1, 2, 3, 4, 5};
    TEST_ASSERT_FALSE(pair_code_from_bytes(b, 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(pair_code_from_bytes(b, 6, out, sizeof(out)));
    TEST_ASSERT_FALSE(pair_code_from_bytes(NULL, sizeof(b), out, sizeof(out)));
    TEST_ASSERT_FALSE(pair_code_from_bytes(b, sizeof(b), out, PAIR_CODE_SIZE - 1));
}

// ---------------------------------------------------------------------- display

static void test_format_groups_four_and_four(void)
{
    char disp[PAIR_CODE_DISPLAY_SIZE];
    TEST_ASSERT_TRUE(pair_code_format("7K3P92QX", disp, sizeof(disp)));
    TEST_ASSERT_EQUAL_STRING("7K3P-92QX", disp);
}

static void test_format_refuses_a_code_of_the_wrong_length(void)
{
    char disp[PAIR_CODE_DISPLAY_SIZE];
    TEST_ASSERT_FALSE(pair_code_format("7K3P92Q", disp, sizeof(disp)));
    TEST_ASSERT_EQUAL_STRING("", disp);
    TEST_ASSERT_FALSE(pair_code_format("7K3P92QXY", disp, sizeof(disp)));
    TEST_ASSERT_FALSE(pair_code_format(NULL, disp, sizeof(disp)));
    TEST_ASSERT_FALSE(pair_code_format("7K3P92QX", disp, sizeof(disp) - 1));
}

// A formatted code has to come back through the normaliser unchanged, or the panel and the page
// disagree about what was shown. This is the round trip that matters.
static void test_the_display_form_normalises_back(void)
{
    char disp[PAIR_CODE_DISPLAY_SIZE];
    const uint8_t b[PAIR_CODE_BYTES] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    TEST_ASSERT_TRUE(pair_code_from_bytes(b, sizeof(b), out, sizeof(out)));

    char canonical[PAIR_CODE_SIZE];
    memcpy(canonical, out, sizeof(canonical));
    TEST_ASSERT_TRUE(pair_code_format(canonical, disp, sizeof(disp)));
    TEST_ASSERT_TRUE(pair_code_normalise(disp, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(canonical, out);
}

// ------------------------------------------------------------------- normalising

static void test_a_canonical_code_is_unchanged(void)
{
    TEST_ASSERT_TRUE(pair_code_normalise("7K3P92QX", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
}

static void test_case_is_folded_up(void)
{
    TEST_ASSERT_TRUE(pair_code_normalise("7k3p92qx", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
    TEST_ASSERT_TRUE(pair_code_normalise("7K3p92Qx", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
}

static void test_separators_are_dropped(void)
{
    TEST_ASSERT_TRUE(pair_code_normalise("7K3P-92QX", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
    TEST_ASSERT_TRUE(pair_code_normalise("  7K3P 92QX\r\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
    TEST_ASSERT_TRUE(pair_code_normalise("-7-K-3-P-9-2-Q-X-", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("7K3P92QX", out);
}

// The whole reason the alphabet is Crockford's. A reader who sees 0 and types O, or sees 1 and
// types l, has made the mistake this panel invites -- and the code they were shown cannot have
// contained O, I, L or U, so the fold is unambiguous rather than a guess.
static void test_o_folds_to_zero_and_i_and_l_fold_to_one(void)
{
    TEST_ASSERT_TRUE(pair_code_normalise("O1234567", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("01234567", out);
    TEST_ASSERT_TRUE(pair_code_normalise("o1234567", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("01234567", out);

    TEST_ASSERT_TRUE(pair_code_normalise("I2345678", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("12345678", out);
    TEST_ASSERT_TRUE(pair_code_normalise("l2345678", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("12345678", out);
    TEST_ASSERT_TRUE(pair_code_normalise("L2345678", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("12345678", out);
}

// U is excluded from the alphabet and is NOT folded. A typed U is a mistake in something else,
// and refusing it says so instead of pairing against a different code.
static void test_u_is_refused_rather_than_folded(void)
{
    TEST_ASSERT_FALSE(pair_code_normalise("U1234567", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(pair_code_normalise("u1234567", out, sizeof(out)));
}

static void test_the_wrong_length_is_refused(void)
{
    TEST_ASSERT_FALSE(pair_code_normalise("7K3P92Q", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(pair_code_normalise("", out, sizeof(out)));
    TEST_ASSERT_FALSE(pair_code_normalise("-", out, sizeof(out)));
}

// Truncation is the dangerous failure here: a normaliser that kept the first eight symbols of a
// longer paste would say a wrong string was the right code.
static void test_too_long_is_refused_not_truncated(void)
{
    TEST_ASSERT_FALSE(pair_code_normalise("7K3P92QXY", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(pair_code_normalise("7K3P92QX7K3P92QX", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_anything_outside_the_alphabet_is_refused(void)
{
    static const char *bad[] = {"7K3P92Q!", "7K3P92Q_", "7K3P9 2Q.", "<script>", "7K3P92Q\x80"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(pair_code_normalise(bad[i], out, sizeof(out)), bad[i]);
        TEST_ASSERT_EQUAL_STRING("", out);
    }
}

static void test_normalise_refuses_bad_buffers(void)
{
    TEST_ASSERT_FALSE(pair_code_normalise("7K3P92QX", out, PAIR_CODE_SIZE - 1));
    TEST_ASSERT_FALSE(pair_code_normalise(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(pair_code_normalise("7K3P92QX", NULL, sizeof(out)));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_five_bytes_become_eight_symbols);
    RUN_TEST(test_the_bit_order_is_most_significant_first);
    RUN_TEST(test_every_symbol_is_in_the_alphabet);
    RUN_TEST(test_from_bytes_refuses_the_wrong_shape);

    RUN_TEST(test_format_groups_four_and_four);
    RUN_TEST(test_format_refuses_a_code_of_the_wrong_length);
    RUN_TEST(test_the_display_form_normalises_back);

    RUN_TEST(test_a_canonical_code_is_unchanged);
    RUN_TEST(test_case_is_folded_up);
    RUN_TEST(test_separators_are_dropped);
    RUN_TEST(test_o_folds_to_zero_and_i_and_l_fold_to_one);
    RUN_TEST(test_u_is_refused_rather_than_folded);
    RUN_TEST(test_the_wrong_length_is_refused);
    RUN_TEST(test_too_long_is_refused_not_truncated);
    RUN_TEST(test_anything_outside_the_alphabet_is_refused);
    RUN_TEST(test_normalise_refuses_bad_buffers);

    return UNITY_END();
}
