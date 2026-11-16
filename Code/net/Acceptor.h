#ifndef _NET_ACCEPTOR_H_
#define _NET_ACCEPTOR_H_

#include <asio.hpp>

#include <functional>
#include <memory>
#include <string>

namespace net
{

// Passive listener. Binds an explicit IPv4 address (or 0.0.0.0) and accepts
// connections on the io_context. Bind failure is reported to the caller so
// startup can release resources and exit non-zero.
class Acceptor : public std::enable_shared_from_this<Acceptor>
{
public:
	using AcceptHandler = std::function<void(asio::ip::tcp::socket, const std::string &peer)>;
	using ErrorHandler = std::function<void(const std::string &error)>;

	explicit Acceptor(asio::io_context &context);
	~Acceptor();

	Acceptor(const Acceptor &) = delete;
	Acceptor &operator=(const Acceptor &) = delete;

	bool Listen(const std::string &address, unsigned short port, std::string &error);

	void Start(AcceptHandler onAccept, ErrorHandler onError);
	void Close();

	bool IsOpen() const { return acceptor_.is_open(); }
	unsigned short LocalPort() const;

private:
	void DoAccept();
	void NotifyError(const std::string &error);

	asio::ip::tcp::acceptor acceptor_;
	AcceptHandler onAccept_;
	ErrorHandler onError_;
};

} // namespace net

#endif
