#include "LocalAddress.h"

#include <asio.hpp>

namespace net
{

std::vector<std::string> LocalIPv4Addresses()
{
	std::vector<std::string> addresses;
	addresses.push_back("127.0.0.1");

	asio::io_context context;
	asio::ip::tcp::resolver resolver(context);

	asio::error_code ec;
	const std::string hostName = asio::ip::host_name(ec);

	if (ec || hostName.empty())
		return addresses;

	const asio::ip::tcp::resolver::results_type results =
		resolver.resolve(hostName, "", asio::ip::tcp::resolver::passive, ec);

	if (ec)
		return addresses;

	for (auto it = results.begin(); it != results.end(); ++it)
	{
		const asio::ip::address address = it->endpoint().address();

		if (!address.is_v4())
			continue;

		const std::string text = address.to_string();

		bool duplicate = false;

		for (const std::string &existing : addresses)
		{
			if (existing == text)
			{
				duplicate = true;
				break;
			}
		}

		if (!duplicate)
			addresses.push_back(text);
	}

	return addresses;
}

ServerGroupMatch MatchServerGroup(const std::vector<std::string> &localAddresses,
                                  const std::vector<std::string> &groupAddresses)
{
	ServerGroupMatch match;

	for (std::size_t local = 0; local < localAddresses.size(); ++local)
	{
		for (std::size_t group = 0; group < groupAddresses.size(); ++group)
		{
			if (groupAddresses[group].empty())
				continue;

			if (localAddresses[local] == groupAddresses[group])
			{
				match.group = static_cast<int>(group);
				match.address = localAddresses[local];
				return match;
			}
		}
	}

	return match;
}

} // namespace net
