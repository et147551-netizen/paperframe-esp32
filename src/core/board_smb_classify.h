// Turning a libsmb2 failure into something a user can act on.
//
// Split out of board_smb.c for one reason: it is a pure function, so it can be linked
// and tested under env:native, which cannot see the smb2/ headers. Ticket
// .scratch/digital-frame/issues/24.
//
// The order of confidence is NT status, then errno name, then which phase was running.
// That order is the ComittoNxA experience ported: its own exception classifier matched
// `endsWith("ConnectException")` and masked a real LOGON_FAILURE three levels down, with
// every JVM test green. So this matches statuses exactly and never by suffix.

#ifndef BOARD_SMB_CLASSIFY_H
#define BOARD_SMB_CLASSIFY_H

typedef enum {
    BOARD_SMB_OK = 0,
    BOARD_SMB_ERR_CONNECT,
    BOARD_SMB_ERR_AUTH,
    BOARD_SMB_ERR_SHARE,
    BOARD_SMB_ERR_NOTFOUND,
    BOARD_SMB_ERR_DENIED,
    // The server stopped answering mid-command and the session was dropped on a deadline.
    // board_smb_classify() never returns this: there is no NT status behind it, because
    // nothing arrived. It comes from board_smb.c's own service loop, and it is a distinct
    // outcome from ERR_CONNECT on purpose -- the host is reachable and the credentials
    // are right, so the mirror's answer is "retry this file later", not "reconfigure".
    BOARD_SMB_ERR_TIMEOUT,
} board_smb_err_t;

// Which call was in flight. The last tier of classification, used when the server sent
// no NT status at all -- two clients authenticating at once makes Samba drop a socket
// and produce a bare "Software caused connection abort" with nothing else in it.
typedef enum {
    BOARD_SMB_PHASE_CONNECT = 0,
    BOARD_SMB_PHASE_LIST,
    BOARD_SMB_PHASE_READ,
} board_smb_phase_t;

// `err` is smb2_get_error()'s string, which is where libsmb2 puts the NT status --
// there is no separate status field to read. NULL and "" are accepted.
board_smb_err_t board_smb_classify(board_smb_phase_t phase, const char *err);

const char *board_smb_err_str(board_smb_err_t e);

#endif // BOARD_SMB_CLASSIFY_H
