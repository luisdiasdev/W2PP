/*
 * Portability shims and the public entry points for the vendored BCrypt
 * implementation. The hashing algorithm itself lives in bcrypt.c/blowfish.c
 * from OpenBSD; see README.md in this directory.
 */

#include "w2pp_bcrypt.h"

#include <stddef.h>
#include <stdint.h>

int bcrypt_newhash(const char *pass, int log_rounds, char *hash, size_t hashlen);
int bcrypt_checkpass(const char *pass, const char *goodhash);

void w2pp_bcrypt_explicit_bzero(void *buf, size_t len)
{
	volatile unsigned char *p = (volatile unsigned char *)buf;

	while (len-- > 0)
		*p++ = 0;
}

int w2pp_bcrypt_timingsafe_bcmp(const void *a, const void *b, size_t len)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	unsigned char result = 0;

	while (len-- > 0)
		result |= *pa++ ^ *pb++;

	return result != 0;
}

int w2pp_bcrypt_hash(const char *password, unsigned int log_rounds,
	char *out, size_t outlen)
{
	if (password == NULL || out == NULL || outlen < W2PP_BCRYPT_HASHSPACE)
		return -1;

	return bcrypt_newhash(password, (int)log_rounds, out, outlen);
}

int w2pp_bcrypt_verify(const char *password, const char *hash)
{
	if (password == NULL || hash == NULL)
		return 0;

	/* Only the two supported prefixes are accepted; reject everything else
	 * before the parser runs so a corrupt column cannot look like a match. */
	if (hash[0] != '$' || hash[1] != '2' || hash[2] == '\0')
		return 0;

	return bcrypt_checkpass(password, hash) == 0 ? 1 : 0;
}
