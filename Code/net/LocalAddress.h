#ifndef _NET_LOCALADDRESS_H_
#define _NET_LOCALADDRESS_H_

#include <string>
#include <vector>

namespace net
{

struct ServerGroupMatch
{
	int group = -1;
	std::string address;
};

// Enumerates the local IPv4 addresses through the asio resolver.
std::vector<std::string> LocalIPv4Addresses();

// Finds the first local address that matches one of the configured group
// addresses. Returns group = -1 when no group matches.
ServerGroupMatch MatchServerGroup(const std::vector<std::string> &localAddresses,
                                  const std::vector<std::string> &groupAddresses);

} // namespace net

#endif
