#ifndef _NET_BILLING_LINK_H_
#define _NET_BILLING_LINK_H_

#include "Connection.h"
#include "Connector.h"
#include "Session.h"

#include <asio.hpp>

#include <functional>
#include <memory>
#include <string>

namespace net
{

// Billing link: unframed 196-byte records with no init handshake.
//
// A lost link resets the legacy 360-tick retry counter (the second timer that
// runs every 500 ms). The owner calls Tick() from that timer; the link
// reconnects when the counter expires and disables itself when a reconnect
// attempt fails, matching the legacy "Reconnect BILL Server(x2) fail" path.
class BillingLink
{
public:
	using RecordHandler = std::function<void(char *record)>;
	using ConnectedHandler = std::function<void()>;
	using LostHandler = std::function<void(const char *reason)>;

	static constexpr int kRetryTicks = 360;

	BillingLink(asio::io_context &context, Connection &connection);
	~BillingLink();

	BillingLink(const BillingLink &) = delete;
	BillingLink &operator=(const BillingLink &) = delete;

	void SetHandlers(RecordHandler onRecord, ConnectedHandler onConnected,
	                 LostHandler onLost);

	// Starts an asynchronous connect. Returns false when the link is already
	// open or the address is empty.
	bool Connect(const std::string &host, unsigned short port,
	             const std::string &bindAddress);

	// Closes the link; the close handler resets the retry counter.
	void Disconnect();

	// One 500 ms timer tick. Returns true when this tick started a reconnect.
	bool Tick();

	// Drops the pending retry (used when billing is disabled).
	void ResetRetry();

	bool IsConnected() const { return connected_; }
	bool IsDisabled() const { return disabled_; }
	int RetryCounter() const { return retryCounter_; }

private:
	void OpenSession(asio::ip::tcp::socket socket);
	void NotifyLost(const std::string &reason);

	asio::io_context &context_;
	Connection &connection_;
	std::shared_ptr<Connector> connector_;

	std::string host_;
	unsigned short port_ = 0;
	std::string bindAddress_;

	RecordHandler onRecord_;
	ConnectedHandler onConnected_;
	LostHandler onLost_;

	int retryCounter_ = 0;
	bool connected_ = false;
	bool connecting_ = false;
	bool disabled_ = false;
};

} // namespace net

#endif
