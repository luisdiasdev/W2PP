/*
 * Smoke executable for the vendored BCrypt target.
 *
 * Hashes a fixed credential at the lowest usable cost (4) so it stays fast,
 * then checks that the correct credential verifies and a wrong one does not.
 * Exits non-zero if the produced hash does not carry the "$2b$" prefix.
 */

#include <cstdio>
#include <cstring>

#include "w2pp_bcrypt.h"

int main()
{
	char hash[W2PP_BCRYPT_HASHSPACE];

	if (w2pp_bcrypt_hash("w2pp-smoke", 4, hash, sizeof(hash)) != 0) {
		std::fprintf(stderr, "bcrypt: hashing failed\n");
		return 1;
	}

	std::printf("%s\n", hash);

	if (std::strncmp(hash, "$2b$04$", 7) != 0) {
		std::fprintf(stderr, "bcrypt: unexpected hash prefix\n");
		return 1;
	}
	if (w2pp_bcrypt_verify("w2pp-smoke", hash) != 1) {
		std::fprintf(stderr, "bcrypt: correct credential did not verify\n");
		return 1;
	}
	if (w2pp_bcrypt_verify("w2pp-wrong", hash) != 0) {
		std::fprintf(stderr, "bcrypt: wrong credential verified\n");
		return 1;
	}

	std::printf("bcrypt smoke test passed\n");
	return 0;
}
