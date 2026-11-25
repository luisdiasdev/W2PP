/*
 * Credential hashing and byte-semantics unit tests (tasks 5.1 and 5.2).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "security/CredentialBytes.h"
#include "security/PasswordHash.h"

TEST_CASE("bcrypt known vector verifies", "[security][hash]")
{
	// Standard $2a$ vector (empty credential); verification accepts both
	// minor versions and the exact legacy byte semantics.
	const std::string hash =
		"$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.";

	REQUIRE(security::PasswordHash::Verify("", hash)
		== security::VerifyResult::Match);
	REQUIRE(security::PasswordHash::Verify("password", hash)
		== security::VerifyResult::Mismatch);
}

TEST_CASE("generated hashes use $2b$ and a fresh salt", "[security][hash]")
{
	std::string first;
	std::string second;
	std::string error;

	REQUIRE(security::PasswordHash::Generate("secret", 10, &first, &error));
	REQUIRE(security::PasswordHash::Generate("secret", 10, &second, &error));

	REQUIRE(first.rfind("$2b$10$", 0) == 0);
	REQUIRE(first != second);
	REQUIRE(first.size() == 60);

	REQUIRE(security::PasswordHash::Verify("secret", first)
		== security::VerifyResult::Match);
	REQUIRE(security::PasswordHash::Verify("secret", second)
		== security::VerifyResult::Match);
	REQUIRE(security::PasswordHash::Verify("Secret", first)
		== security::VerifyResult::Mismatch);
}

TEST_CASE("cost validation rejects weak and excessive factors", "[security][hash]")
{
	std::string error;
	std::string hash;

	REQUIRE_FALSE(security::PasswordHash::ValidateCost(9, &error));
	REQUIRE(error.find("10") != std::string::npos);
	REQUIRE_FALSE(security::PasswordHash::ValidateCost(15, &error));
	REQUIRE_FALSE(security::PasswordHash::Generate("x", 9, &hash, &error));

	REQUIRE(security::PasswordHash::ValidateCost(10, &error));
	REQUIRE(security::PasswordHash::ValidateCost(14, &error));
}

TEST_CASE("malformed hashes are reported, not matched", "[security][hash]")
{
	REQUIRE(security::PasswordHash::Verify("x", "plaintext")
		== security::VerifyResult::MalformedHash);
	REQUIRE(security::PasswordHash::Verify("x", "$2b$99$abcdef")
		== security::VerifyResult::MalformedHash);
	REQUIRE(security::PasswordHash::Verify("x", "")
		== security::VerifyResult::MalformedHash);
}

TEST_CASE("needs-rehash detects cost and format", "[security][hash]")
{
	REQUIRE(security::PasswordHash::NeedsRehash(
		"$2a$10$abcdefghijklmnopqrstuv", 10));
	REQUIRE(security::PasswordHash::NeedsRehash(
		"$2b$10$abcdefghijklmnopqrstuv", 11));
	REQUIRE_FALSE(security::PasswordHash::NeedsRehash(
		"$2b$12$abcdefghijklmnopqrstuv", 12));
	REQUIRE(security::PasswordHash::NeedsRehash("garbage", 12));
}

TEST_CASE("credential byte extraction follows the legacy field semantics",
	"[security][bytes]")
{
	char field[16];

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "fifteenchars123", 15); // 15 bytes + NUL
	REQUIRE(security::SubmittedCredential(field, 16) == "fifteenchars123");
	REQUIRE(security::NewCredential(field, 16) == "fifteenchars123");

	// 16 bytes without a NUL stays byte-exact for verification.
	std::memcpy(field, "sixteenchars1234", 16);
	REQUIRE(security::SubmittedCredential(field, 16) == "sixteenchars1234");
	REQUIRE(security::NewCredential(field, 16) == "sixteenchars123");

	// Case is preserved; no trimming happens.
	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "  Secret  ", 10);
	REQUIRE(security::SubmittedCredential(field, 16) == "  Secret  ");

	// An embedded NUL terminates the credential.
	std::memset(field, 0, sizeof(field));
	field[0] = 'a';
	field[1] = 'b';
	field[2] = '\0';
	field[3] = 'c';
	REQUIRE(security::SubmittedCredential(field, 16) == "ab");
}

TEST_CASE("legacy prefix markers map to account states", "[security][bytes]")
{
	char field[16];

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "@blocked", 8);
	security::LegacyCredential blocked =
		security::SplitLegacyCredential(field, 16);
	REQUIRE(blocked.state == security::AccountState::Blocked);
	REQUIRE(blocked.credential == "blocked");
	REQUIRE(blocked.usable);

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "_deleted", 8);
	security::LegacyCredential deleted =
		security::SplitLegacyCredential(field, 16);
	REQUIRE(deleted.state == security::AccountState::Deleted);
	REQUIRE(deleted.credential == "deleted");

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "#disabled", 9);
	security::LegacyCredential disabled =
		security::SplitLegacyCredential(field, 16);
	REQUIRE(disabled.state == security::AccountState::Disabled);
	REQUIRE(disabled.credential == "disabled");

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "normal", 6);
	security::LegacyCredential normal =
		security::SplitLegacyCredential(field, 16);
	REQUIRE(normal.state == security::AccountState::Normal);
	REQUIRE(normal.credential == "normal");

	std::memset(field, 0, sizeof(field));
	std::memcpy(field, "@", 1);
	security::LegacyCredential empty =
		security::SplitLegacyCredential(field, 16);
	REQUIRE_FALSE(empty.usable);
}

TEST_CASE("hash detection avoids double hashing on import", "[security][bytes]")
{
	REQUIRE(security::LooksHashed(
		"$2b$12$abcdefghijklmnopqrstuv"));
	REQUIRE(security::LooksHashed(
		"$2a$10$abcdefghijklmnopqrstuv"));
	REQUIRE_FALSE(security::LooksHashed("hunter2"));
	REQUIRE_FALSE(security::LooksHashed("$2"));
}
