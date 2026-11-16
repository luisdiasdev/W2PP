#ifndef _NET_CONNECTION_H_
#define _NET_CONNECTION_H_

#include "Session.h"

#include <cstdint>

namespace net
{

// Legacy send-surface facade over net::Session.
//
// The names match CPSock's public methods so the existing SendOneMessage /
// AddMessage / SendMessageA / CloseSocket call sites do not change.
class Connection
{
public:
	Connection() = default;
	explicit Connection(SessionPtr session);

	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;

	void Attach(SessionPtr session);
	void Detach();
	SessionPtr GetSession() const { return session_; }

	bool SendOneMessage(char *msg, int size);
	bool AddMessage(char *msg, int size);
	bool SendMessageA();
	bool SendBillMessage(char *msg);
	bool CloseSocket();

	bool IsOpen() const;
	std::uint64_t Generation() const;
	std::size_t QueuedBytes() const;

	// True while the attached session is still the one that produced
	// `generation`. Callbacks capture the generation they were created with so
	// a recycled slot cannot deliver events to a newer connection.
	bool IsCurrent(std::uint64_t generation) const
	{
		return generation != 0 && Generation() == generation;
	}

private:
	SessionPtr session_;
};

} // namespace net

#endif
