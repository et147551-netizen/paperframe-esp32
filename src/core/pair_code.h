// The alphabet and the encoder for a short secret a person reads off the panel and types in.
//
// **Two things use it**: the pairing code, which is what this file was written for, and since
// 2026-09-19 the access point's WPA2 password, which the operator asked to be as short as it can
// be because it is typed by hand (both ticket 66; `app_auth.h` carries the entropy arithmetic).
// The file keeps its name because the pairing code is the one with rules of its own below.
//
// Ticket 66. The pairing code exists because a PC has no camera and the pairing QR was the only way
// the API token was ever handed out, so a desktop browser could not pair at all. **It is not a
// second authentication scheme** -- it is a delivery channel for the one permanent token app_auth.c
// already mints, which is what answers ticket 29's rejection of a PIN ("needs rate limiting and
// a session table"): there is no session, and the rate limiting is a five-attempt counter on a
// window that only a physical button hold opens.
//
// **Crockford base32, and that is the whole reason this file exists rather than eight hex
// characters.** The code is read off a six-colour panel through an ordered dither at three or
// four pixels a module: the alphabet leaves out I, L, O and U, and the *input* side folds the
// confusions a reader makes anyway -- I and l to 1, O to 0 -- so a misread is corrected rather
// than refused. Hex would have offered 0/O and 1/l with no way to tell which was meant.
//
// **The folding is the half the Wi-Fi password does NOT get.** A pairing code is compared by
// app_auth.c, which can forgive O for 0; a WPA2 passphrase is compared byte-for-byte by the
// supplicant, so for that use the ambiguity has to be gone from the *alphabet* and cannot be
// repaired on the way in. Excluding I, L, O and U is what serves both.
//
// It also happens to be 5 bits a character where hex is 4, so the same eight typed characters carry
// 40 bits instead of 32 -- which is why the password got shorter without getting eight times weaker.
//
// Pure, so this half is host-tested (test/test_pair_code); the randomness and the window live in
// src/app/app_auth.c.

#ifndef PAIR_CODE_H
#define PAIR_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Eight symbols, 5 bits each: 2^40. Five guesses per button press is what makes that ample.
#define PAIR_CODE_LEN 8
#define PAIR_CODE_SIZE (PAIR_CODE_LEN + 1)
// "XXXX-XXXX"
#define PAIR_CODE_DISPLAY_SIZE (PAIR_CODE_LEN + 2)
// Bytes of randomness PAIR_CODE_LEN symbols consume.
#define PAIR_CODE_BYTES 5

// Crockford's 32, in value order. I, L, O and U are absent by construction.
#define PAIR_CODE_ALPHABET "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

// `bytes` (PAIR_CODE_BYTES of them) to a canonical code. False on a short buffer or the wrong
// byte count, and `out` is then left empty rather than half-written.
bool pair_code_from_bytes(const uint8_t *bytes, size_t n, char *out, size_t size);

// A canonical code to its display form, "XXXX-XXXX". The group of four is there because eight
// unbroken symbols are read back wrongly; the hyphen is stripped again on the way in.
bool pair_code_format(const char *code, char *out, size_t size);

// Whatever the user typed to a canonical code: case folded, `-` and whitespace dropped, O to 0,
// I and L to 1. True only when the result is exactly PAIR_CODE_LEN symbols of the alphabet --
// so a caller can treat false as "not even the shape of a code" and never compare it.
bool pair_code_normalise(const char *in, char *out, size_t size);

#endif // PAIR_CODE_H
