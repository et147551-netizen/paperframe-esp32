// See board_smb_classify.h. Every string in the table below that carries a measurement
// note was produced by env:smbprobe against a real share on 2026-09-04; the rest come
// from ticket 24's table and are marked as untested.

#include "board_smb_classify.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// NT statuses, matched on the number libsmb2 prints in its error string.
static const struct {
    unsigned long status;
    board_smb_err_t err;
} NT_STATUS[] = {
    // AUTH. 0xC000006D measured: "Session setup failed with (0xc000006d)
    // STATUS_LOGON_FAILURE", 2.08-2.17 s over three rounds.
    {0xC000006DUL, BOARD_SMB_ERR_AUTH}, // LOGON_FAILURE            (measured)
    {0xC000006AUL, BOARD_SMB_ERR_AUTH}, // WRONG_PASSWORD
    {0xC0000064UL, BOARD_SMB_ERR_AUTH}, // NO_SUCH_USER
    {0xC0000071UL, BOARD_SMB_ERR_AUTH}, // PASSWORD_EXPIRED
    {0xC0000072UL, BOARD_SMB_ERR_AUTH}, // ACCOUNT_DISABLED
    {0xC0000234UL, BOARD_SMB_ERR_AUTH}, // ACCOUNT_LOCKED_OUT
    {0xC000015BUL, BOARD_SMB_ERR_AUTH}, // LOGON_TYPE_NOT_GRANTED

    // SHARE. 0xC00000CC measured: "Tree Connect failed with (0xc00000cc)
    // STATUS_BAD_NETWORK_NAME.", 72-128 ms. This is the wrong-share-name signal, and
    // getting it to the user is most of the value of this table.
    {0xC00000CCUL, BOARD_SMB_ERR_SHARE},    // BAD_NETWORK_NAME     (measured)
    {0xC00000C9UL, BOARD_SMB_ERR_SHARE},    // NETWORK_NAME_DELETED

    // DENIED. 0xC0000022 measured at the list phase: "Opendir failed with (0xc0000022)
    // STATUS_ACCESS_DENIED.", 11-24 ms, against a share the account cannot read.
    {0xC0000022UL, BOARD_SMB_ERR_DENIED},   // ACCESS_DENIED        (measured)
    {0xC00000CAUL, BOARD_SMB_ERR_DENIED},   // NETWORK_ACCESS_DENIED

    // NOTFOUND. Not yet reproduced on hardware -- the run that would have done it lost
    // its association (reason=4) before reaching the share.
    {0xC000000FUL, BOARD_SMB_ERR_NOTFOUND}, // NO_SUCH_FILE
    {0xC0000034UL, BOARD_SMB_ERR_NOTFOUND}, // OBJECT_NAME_NOT_FOUND
    {0xC000003AUL, BOARD_SMB_ERR_NOTFOUND}, // OBJECT_PATH_NOT_FOUND
};

// The last tier before the phase fallback. Locale-independent names, because libsmb2
// prints strerror() output in places and that is translated on some platforms.
static const struct {
    const char *needle;
    board_smb_err_t err;
} ERRNO_NAME[] = {
    {"ENETUNREACH", BOARD_SMB_ERR_CONNECT},
    {"EHOSTUNREACH", BOARD_SMB_ERR_CONNECT},
    {"ECONNREFUSED", BOARD_SMB_ERR_CONNECT},
    {"ETIMEDOUT", BOARD_SMB_ERR_CONNECT},
    // Measured against an address with no host on it: libsmb2's own deadline fires and
    // says this, with no NT status and no errno name. 11.11 s at smb2_set_timeout(10).
    {"Timeout expired", BOARD_SMB_ERR_CONNECT},
};

// Finds the first "(0x........)" in `s` and returns its value. Deliberately strict: an
// NT status is exactly eight hex digits inside parentheses, and anything else is not one.
static bool nt_status_of(const char *s, unsigned long *out)
{
    for (const char *p = s; (p = strstr(p, "(0x")) != NULL; p++) {
        const char *h = p + 3;
        unsigned long value = 0;
        int digits = 0;
        while (digits < 8) {
            const char c = h[digits];
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = (unsigned)(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = (unsigned)(c - 'A') + 10u;
            } else {
                break;
            }
            value = (value << 4) | d;
            digits++;
        }
        if (digits == 8 && h[8] == ')') {
            *out = value;
            return true;
        }
    }
    return false;
}

board_smb_err_t board_smb_classify(board_smb_phase_t phase, const char *err)
{
    if (err != NULL && err[0] != '\0') {
        unsigned long status = 0;
        if (nt_status_of(err, &status)) {
            for (size_t i = 0; i < sizeof(NT_STATUS) / sizeof(NT_STATUS[0]); i++) {
                if (NT_STATUS[i].status == status) {
                    return NT_STATUS[i].err;
                }
            }
            // An unmapped status is still the server answering and refusing, so the
            // transport is fine and "cannot reach the server" would be a lie. Which of
            // the remaining messages is least wrong depends on the phase, and this is a
            // judgement call rather than a measurement -- which is why the caller logs
            // the raw string as well.
            switch (phase) {
            case BOARD_SMB_PHASE_LIST:
            case BOARD_SMB_PHASE_READ:
                return BOARD_SMB_ERR_NOTFOUND;
            case BOARD_SMB_PHASE_CONNECT:
            default:
                return BOARD_SMB_ERR_AUTH;
            }
        }

        for (size_t i = 0; i < sizeof(ERRNO_NAME) / sizeof(ERRNO_NAME[0]); i++) {
            if (strstr(err, ERRNO_NAME[i].needle) != NULL) {
                return ERRNO_NAME[i].err;
            }
        }
    }

    // No NT status and no errno name. ComittoNxA's case for this is a bare "Software
    // caused connection abort" when two clients authenticate at once and Samba drops one
    // socket -- a transport failure at whatever phase it happened to interrupt.
    (void)phase;
    return BOARD_SMB_ERR_CONNECT;
}

const char *board_smb_err_str(board_smb_err_t e)
{
    switch (e) {
    case BOARD_SMB_OK:
        return "ok";
    case BOARD_SMB_ERR_CONNECT:
        return "cannot reach the server";
    case BOARD_SMB_ERR_AUTH:
        return "wrong user name or password";
    case BOARD_SMB_ERR_SHARE:
        return "no such share on that server";
    case BOARD_SMB_ERR_NOTFOUND:
        return "no such file or folder";
    case BOARD_SMB_ERR_DENIED:
        return "access denied";
    case BOARD_SMB_ERR_TIMEOUT:
        return "the server stopped responding";
    }
    return "unknown";
}
