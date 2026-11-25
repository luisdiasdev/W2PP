/*
 * Small C entry points around the vendored OpenBSD bcrypt implementation.
 *
 * The library target w2pp_bcrypt exposes exactly these two functions; the
 * DBSrv security module wraps them in C++ (Code/security/PasswordHash).
 */

#ifndef W2PP_THIRD_PARTY_BCRYPT_H
#define W2PP_THIRD_PARTY_BCRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest hash the implementation writes, including the terminating NUL. */
#define W2PP_BCRYPT_HASHSPACE 61

/*
 * Hash `password` with the given log2 cost factor. Writes a "$2b$<cost>$..."
 * string (NUL terminated) to `out`. Returns 0 on success, -1 on failure
 * (invalid arguments or an unsupported cost).
 */
int w2pp_bcrypt_hash(const char *password, unsigned int log_rounds,
	char *out, size_t outlen);

/*
 * Verify `password` against a "$2a$" or "$2b$" hash. Returns 1 on a match,
 * 0 on a mismatch or an unusable hash. Never writes to the hash.
 */
int w2pp_bcrypt_verify(const char *password, const char *hash);

#ifdef __cplusplus
}
#endif

#endif /* W2PP_THIRD_PARTY_BCRYPT_H */
