// The switch for src/smb_aes_hw.c's linker wrap. See that file for what the wrap is for.
//
// It exists so that "hardware AES is 1.6x" is a measurement rather than an interpolation
// across two runs, and so that "depth > 1 fails at the shipping TCP window" can be tested
// with the wrap OFF -- those failures have only ever been observed with it on, which leaves
// the wrap itself as an unexcluded cause. One flash, one association, one session policy,
// one variable.
//
// Nothing but env:smbprobe should call this. The shipping application leaves it on.

#ifndef SMB_AES_HW_H
#define SMB_AES_HW_H

#include <stdbool.h>

// true (the default) routes libsmb2's per-block AES to the AES peripheral; false hands it
// straight back to the component's own portable-C implementation via __real_.
void smb_aes_hw_set_enabled(bool on);
bool smb_aes_hw_enabled(void);

#endif // SMB_AES_HW_H
