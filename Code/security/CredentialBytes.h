/*
 * Credential byte semantics and legacy account-state markers.
 *
 * Verification uses the exact bytes the legacy server accepted: at most 16
 * bytes from a fixed password field, terminated at the first NUL, compared
 * case-sensitively without trimming. New passwords are still limited to 15
 * bytes. Numeric tokens use their 6-byte field byte-for-byte.
 */

#ifndef W2PP_SECURITY_CREDENTIALBYTES_H
#define W2PP_SECURITY_CREDENTIALBYTES_H

#include <cstddef>
#include <string>

namespace security
{

// Values stored in accounts.state and reported as NP state 0..3.
enum class AccountState : int
{
	Normal = 0,
	Blocked = 1,
	Deleted = 2,
	Disabled = 3
};

const char *AccountStateName(AccountState state);

struct LegacyCredential
{
	AccountState state = AccountState::Normal;
	std::string credential;
	bool usable = false;
};

// Split a legacy password field into the prefix state marker and the credential
// bytes that follow it. `usable` is false for an empty credential or one that
// cannot be hashed; callers import those accounts locked and report the reason.
LegacyCredential SplitLegacyCredential(const char *field, std::size_t capacity);

// Verification input: up to `capacity` bytes (16 for passwords, 6 for tokens),
// stopped at the first NUL. Never trimmed or case-folded.
std::string SubmittedCredential(const char *field, std::size_t capacity);

// Account creation / password change: at most 15 bytes, stopped at the first
// NUL, matching the legacy client limit.
std::string NewCredential(const char *field, std::size_t capacity);

// True when the value already looks like a BCrypt hash and must not be hashed
// again during an import re-run.
bool LooksHashed(const std::string &value);

}

#endif
