#include "Acceptor.h"

#include <system_error>
#include <utility>

namespace net
{

Acceptor::Acceptor(asio::io_context &context)
	: acceptor_(context)
{
}

Acceptor::~Acceptor()
{
	Close();
}

bool Acceptor::Listen(const std::string &address, unsigned short port, std::string &error)
{
	error.clear();

	asio::error_code ec;
	const asio::ip::address bindAddress = asio::ip::make_address(address, ec);

	if (ec)
	{
		error = "invalid listen address: " + address;
		return false;
	}

	const asio::ip::tcp::endpoint endpoint(bindAddress, port);

	acceptor_.open(endpoint.protocol(), ec);

	if (ec)
	{
		error = ec.message();
		return false;
	}

	acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
	acceptor_.bind(endpoint, ec);

	if (ec)
	{
		error = ec.message();
		acceptor_.close();
		return false;
	}

	acceptor_.listen(asio::socket_base::max_listen_connections, ec);

	if (ec)
	{
		error = ec.message();
		acceptor_.close();
		return false;
	}

	return true;
}

void Acceptor::Start(AcceptHandler onAccept, ErrorHandler onError)
{
	onAccept_ = std::move(onAccept);
	onError_ = std::move(onError);
	DoAccept();
}

void Acceptor::DoAccept()
{
	if (!acceptor_.is_open())
		return;

	std::shared_ptr<Acceptor> self = shared_from_this();

	acceptor_.async_accept(
		[this, self](const asio::error_code &error, asio::ip::tcp::socket socket)
		{
			if (!error)
			{
				const asio::ip::tcp::endpoint endpoint = socket.remote_endpoint();
				const std::string peer = endpoint.address().to_string();

				if (onAccept_)
					onAccept_(std::move(socket), peer);
			}
			else if (error != asio::error::operation_aborted)
			{
				NotifyError(error.message());
			}

			if (acceptor_.is_open())
				DoAccept();
		});
}

void Acceptor::Close()
{
	if (!acceptor_.is_open())
		return;

	asio::error_code ec;
	acceptor_.close(ec);
}

unsigned short Acceptor::LocalPort() const
{
	asio::error_code ec;
	const asio::ip::tcp::endpoint endpoint = acceptor_.local_endpoint(ec);
	return ec ? 0 : endpoint.port();
}

void Acceptor::NotifyError(const std::string &error)
{
	if (onError_)
		onError_(error);
}

} // namespace net
