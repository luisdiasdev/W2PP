#include "PasswordHash.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "w2pp_bcrypt.h"

namespace security
{
namespace
{

bool ParseCost(const std::string &hash, int *cost, char *minor)
{
	// Canonical shape: $2X$NN$... with two decimal digits.
	if (hash.size() < 7 || hash[0] != '$' || hash[1] != '2' || hash[3] != '$')
		return false;

	const char marker = hash[2];
	if (marker != 'a' && marker != 'b' && marker != 'x' && marker != 'y')
		return false;

	if (!std::isdigit(static_cast<unsigned char>(hash[4]))
		|| !std::isdigit(static_cast<unsigned char>(hash[5]))
		|| hash[6] != '$')
		return false;

	const int value = (hash[4] - '0') * 10 + (hash[5] - '0');

	if (value < 0 || value > 31)
		return false;

	if (cost != nullptr)
		*cost = value;

	if (minor != nullptr)
		*minor = marker;

	return true;
}

} // namespace

bool PasswordHash::ValidateCost(int cost, std::string *error)
{
	if (cost < kMinCost || cost > kMaxCost)
	{
		if (error != nullptr)
		{
			*error = "BcryptCost must be between " + std::to_string(kMinCost)
				+ " and " + std::to_string(kMaxCost);
		}
		return false;
	}

	return true;
}

bool PasswordHash::Generate(const std::string &credential, int cost,
	std::string *hash, std::string *error)
{
	if (hash == nullptr)
		return false;

	if (!ValidateCost(cost, error))
		return false;

	char buffer[W2PP_BCRYPT_HASHSPACE];

	if (w2pp_bcrypt_hash(credential.c_str(), static_cast<unsigned int>(cost),
			buffer, sizeof(buffer)) != 0)
	{
		if (error != nullptr)
			*error = "bcrypt hashing failed";
		return false;
	}

	*hash = buffer;
	return true;
}

VerifyResult PasswordHash::Verify(const std::string &credential,
	const std::string &hash)
{
	int cost = 0;

	if (!ParseCost(hash, &cost, nullptr))
		return VerifyResult::MalformedHash;

	return w2pp_bcrypt_verify(credential.c_str(), hash.c_str()) == 1
		? VerifyResult::Match : VerifyResult::Mismatch;
}

bool PasswordHash::NeedsRehash(const std::string &hash, int cost)
{
	int storedCost = 0;
	char minor = 'a';

	if (!ParseCost(hash, &storedCost, &minor))
		return true;

	return storedCost < cost || minor != 'b';
}

bool PasswordHash::HashCost(const std::string &hash, int *cost)
{
	return ParseCost(hash, cost, nullptr);
}

}
