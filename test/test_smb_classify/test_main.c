// Host-side tests for ticket 24's failure classifier.
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// Every string marked MEASURED below was copied out of a capture log verbatim, including
// its trailing full stop and the lower-case hex -- env:smbprobe against a real Windows
// share on 2026-09-04, .scratch/captures/smbfail-*.log. Strings invented to look like a
// server's are the thing this file exists to avoid: the classifier that shipped in
// ComittoNxA passed every JVM test and still masked a LOGON_FAILURE on hardware.

#include <stdio.h>

#include <unity.h>

#include "board_smb_classify.h"

void setUp(void) {}
void tearDown(void) {}

static void test_logon_failure_is_auth(void)
{
    // MEASURED: wrong password, 2.08-2.17 s, rc=-111.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_AUTH,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT,
                                         "Session setup failed with (0xc000006d) "
                                         "STATUS_LOGON_FAILURE"));
}

static void test_bad_network_name_is_share(void)
{
    // MEASURED: share that does not exist, 72-128 ms, rc=-2. Note the trailing period.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_SHARE,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT,
                                         "Tree Connect failed with (0xc00000cc) "
                                         "STATUS_BAD_NETWORK_NAME. "));
}

static void test_access_denied_is_denied(void)
{
    // MEASURED: a share the account cannot read, failing at opendir, 11-24 ms.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_DENIED,
                      board_smb_classify(BOARD_SMB_PHASE_LIST,
                                         "Opendir failed with (0xc0000022) "
                                         "STATUS_ACCESS_DENIED."));
}

static void test_libsmb2_timeout_is_connect(void)
{
    // MEASURED: an address with no host on it. libsmb2's own deadline, 11.11 s at
    // smb2_set_timeout(10). No NT status, no errno name -- only this sentence.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT,
                                         "Timeout expired and no connection exists"));
}

static void test_not_found_statuses(void)
{
    // MEASURED: a path that does not exist inside a share that does, failing at opendir,
    // 11.4-15.5 ms over three rounds.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_NOTFOUND,
                      board_smb_classify(BOARD_SMB_PHASE_LIST,
                                         "Opendir failed with (0xc0000034) "
                                         "STATUS_OBJECT_NAME_NOT_FOUND."));
    // NOT measured: the same status seen at the read phase.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_NOTFOUND,
                      board_smb_classify(BOARD_SMB_PHASE_READ,
                                         "Open failed with (0xc0000034) "
                                         "STATUS_OBJECT_NAME_NOT_FOUND"));
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_NOTFOUND,
                      board_smb_classify(BOARD_SMB_PHASE_READ,
                                         "Open failed with (0xc000003a) "
                                         "STATUS_OBJECT_PATH_NOT_FOUND"));
}

static void test_status_wins_over_the_phase(void)
{
    // A LOGON_FAILURE seen while listing is still an auth problem. This is the ComittoNxA
    // bug in one assertion: their classifier let the throw site win and reported a
    // connection error for exactly this case.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_AUTH,
                      board_smb_classify(BOARD_SMB_PHASE_LIST,
                                         "Session setup failed with (0xc000006d) "
                                         "STATUS_LOGON_FAILURE"));
}

static void test_errno_names_are_matched_exactly(void)
{
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT,
                                         "connect failed: ECONNREFUSED"));
    // A message that merely contains a similar word is not a match. "ETIMEDOUTish" would
    // be caught by a substring test and this asserts the opposite is not true of the
    // status tier: no status, no errno name -> transport.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_READ,
                                         "Software caused connection abort"));
}

static void test_malformed_status_is_not_a_status(void)
{
    // Seven digits, not eight, so it is not an NT status and must not be read as one.
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT,
                                         "failed with (0xc00006d) STATUS_LOGON_FAILURE"));
}

static void test_empty_and_null(void)
{
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT, NULL));
    TEST_ASSERT_EQUAL(BOARD_SMB_ERR_CONNECT,
                      board_smb_classify(BOARD_SMB_PHASE_CONNECT, ""));
}

static void test_every_code_has_a_message(void)
{
    const board_smb_err_t all[] = {BOARD_SMB_OK,        BOARD_SMB_ERR_CONNECT,
                                   BOARD_SMB_ERR_AUTH,  BOARD_SMB_ERR_SHARE,
                                   BOARD_SMB_ERR_NOTFOUND, BOARD_SMB_ERR_DENIED,
                                   BOARD_SMB_ERR_TIMEOUT};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        TEST_ASSERT_NOT_NULL(board_smb_err_str(all[i]));
        TEST_ASSERT_NOT_EQUAL(0, board_smb_err_str(all[i])[0]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_logon_failure_is_auth);
    RUN_TEST(test_bad_network_name_is_share);
    RUN_TEST(test_access_denied_is_denied);
    RUN_TEST(test_libsmb2_timeout_is_connect);
    RUN_TEST(test_not_found_statuses);
    RUN_TEST(test_status_wins_over_the_phase);
    RUN_TEST(test_errno_names_are_matched_exactly);
    RUN_TEST(test_malformed_status_is_not_a_status);
    RUN_TEST(test_empty_and_null);
    RUN_TEST(test_every_code_has_a_message);
    return UNITY_END();
}
