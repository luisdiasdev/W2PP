#include "CredentialBytes.h"

#include <cstring>

namespace security
{
namespace
{

std::string BytesUntilNul(const char *field, std::size_t capacity,
	std::size_t maximum)
{
	std::string value;

	if (field == nullptr || capacity == 0 || maximum == 0)
		return value;

	std::size_t length = 0;
	while (length < capacity && length < maximum && field[length] != '\0')
		++length;

	value.assign(field, length);
	return value;
}

} // namespace

const char *AccountStateName(AccountState state)
{
	switch (state)
	{
	case AccountState::Normal:
		return "normal";
	case AccountState::Blocked:
		return "blocked";
	case AccountState::Deleted:
		return "deleted";
	case AccountState::Disabled:
		return "disabled";
	}

	return "unknown";
}

LegacyCredential SplitLegacyCredential(const char *field, std::size_t capacity)
{
	LegacyCredential result;

	if (field == nullptr || capacity == 0)
		return result;

	std::size_t offset = 0;
	unsigned char marker = static_cast<unsigned char>(field[0]);

	if (marker == '@')
		result.state = AccountState::Blocked;
	else if (marker == '_')
		result.state = AccountState::Deleted;
	else if (marker == '#')
		result.state = AccountState::Disabled;

	if (marker == '@' || marker == '_' || marker == '#')
		offset = 1;

	result.credential = BytesUntilNul(field + offset, capacity - offset, 16);
	result.usable = !result.credential.empty();

	return result;
}

std::string SubmittedCredential(const char *field, std::size_t capacity)
{
	return BytesUntilNul(field, capacity, capacity);
}

std::string NewCredential(const char *field, std::size_t capacity)
{
	// One byte of headroom for the implicit NUL the legacy client assumes.
	const std::size_t maximum = capacity > 0 ? capacity - 1 : 0;
	return BytesUntilNul(field, capacity, maximum);
}

bool LooksHashed(const std::string &value)
{
	return value.size() >= 7 && value[0] == '$' && value[1] == '2'
		&& value[3] == '$';
}

}
