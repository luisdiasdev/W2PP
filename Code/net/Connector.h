#ifndef _NET_CONNECTOR_H_
#define _NET_CONNECTOR_H_

#include <asio.hpp>

#include <functional>
#include <memory>
#include <string>

namespace net
{

// Outbound link: resolves an IPv4 endpoint, binds a local address (with the
// legacy local-port fallback ladder), connects, and optionally sends the
// four-byte init handshake before handing the socket to the caller.
class Connector : public std::enable_shared_from_this<Connector>
{
public:
	using ConnectHandler = std::function<void(asio::ip::tcp::socket)>;
	using ErrorHandler = std::function<void(const std::string &error)>;

	explicit Connector(asio::io_context &context);
	~Connector();

	Connector(const Connector &) = delete;
	Connector &operator=(const Connector &) = delete;

	void Connect(const std::string &host, unsigned short port,
	             const std::string &bindAddress, bool sendInitCode,
	             ConnectHandler onConnected, ErrorHandler onError);

	void Cancel();

private:
	void Resolve(const std::string &host, unsigned short port);
	void ConnectTo(const asio::ip::tcp::endpoint &endpoint);

	asio::io_context &context_;
	asio::ip::tcp::resolver resolver_;
	bool cancelled_ = false;

	std::string bindAddress_;
	bool sendInitCode_ = false;

	ConnectHandler onConnected_;
	ErrorHandler onError_;
};

} // namespace net

#endif
