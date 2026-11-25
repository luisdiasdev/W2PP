/*
 * BCrypt password/token hashing policy used by DBSrv only.
 *
 * The vendored implementation lives in third_party/bcrypt; this wrapper adds
 * the project policy: cost validation (10..14, default 12), prefix detection
 * and malformed-hash reporting.
 */

#ifndef W2PP_SECURITY_PASSWORDHASH_H
#define W2PP_SECURITY_PASSWORDHASH_H

#include <string>

namespace security
{

enum class VerifyResult
{
	Match,
	Mismatch,
	MalformedHash
};

class PasswordHash
{
public:
	// Accepted policy range; mirrors StorageConfig::kMin/MaxBcryptCost.
	static const int kMinCost = 10;
	static const int kMaxCost = 14;

	// False and an actionable message when cost is outside 10..14.
	static bool ValidateCost(int cost, std::string *error);

	// Hash a credential into a fresh "$2b$<cost>$..." string.
	static bool Generate(const std::string &credential, int cost,
		std::string *hash, std::string *error);

	static VerifyResult Verify(const std::string &credential,
		const std::string &hash);

	// True when the hash's cost is below `cost` or its format is not $2b$.
	static bool NeedsRehash(const std::string &hash, int cost);

	// Parse "$2a$NN$" / "$2b$NN$". False when the value is not a BCrypt hash.
	static bool HashCost(const std::string &hash, int *cost);
};

}

#endif
