#include "Connector.h"

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace net
{
namespace
{

const unsigned short kLocalBindPorts[] = { 0, 5000, 5010, 5020 };

} // namespace

Connector::Connector(asio::io_context &context)
	: context_(context)
	, resolver_(context)
{
}

Connector::~Connector()
{
	Cancel();
}

void Connector::Connect(const std::string &host, unsigned short port,
                        const std::string &bindAddress, bool sendInitCode,
                        ConnectHandler onConnected, ErrorHandler onError)
{
	cancelled_ = false;
	bindAddress_ = bindAddress;
	sendInitCode_ = sendInitCode;
	onConnected_ = std::move(onConnected);
	onError_ = std::move(onError);

	Resolve(host, port);
}

void Connector::Resolve(const std::string &host, unsigned short port)
{
	std::shared_ptr<Connector> self = shared_from_this();

	resolver_.async_resolve(
		host, std::to_string(port),
		[this, self](const asio::error_code &error,
		             const asio::ip::tcp::resolver::results_type &results)
		{
			if (cancelled_)
				return;

			if (error)
			{
				if (onError_)
					onError_(error.message());
				return;
			}

			if (results.empty())
			{
				if (onError_)
					onError_("no address for host");
				return;
			}

			for (auto it = results.begin(); it != results.end(); ++it)
			{
				if (it->endpoint().address().is_v4())
				{
					ConnectTo(it->endpoint());
					return;
				}
			}

			ConnectTo(results.begin()->endpoint());
		});
}

void Connector::ConnectTo(const asio::ip::tcp::endpoint &endpoint)
{

	std::shared_ptr<asio::ip::tcp::socket> socket =
		std::make_shared<asio::ip::tcp::socket>(context_);

	asio::error_code ec;
	asio::ip::address bindAddress = asio::ip::address_v4::any();

	if (!bindAddress_.empty())
	{
		const asio::ip::address parsed = asio::ip::make_address(bindAddress_, ec);

		if (!ec)
			bindAddress = parsed;
	}

	bool bound = false;

	for (unsigned short localPort : kLocalBindPorts)
	{
		socket->open(endpoint.protocol(), ec);

		if (ec)
			break;

		const asio::ip::tcp::endpoint local(bindAddress, localPort);
		socket->bind(local, ec);

		if (!ec)
		{
			bound = true;
			break;
		}

		socket->close();
	}

	if (!bound)
	{
		if (onError_)
			onError_("local bind failed");

		return;
	}

	std::shared_ptr<Connector> self = shared_from_this();

	std::shared_ptr<asio::steady_timer> timeout =
		std::make_shared<asio::steady_timer>(context_);
	std::shared_ptr<bool> finished = std::make_shared<bool>(false);

	timeout->expires_after(std::chrono::milliseconds(2000));
	timeout->async_wait(
		[this, self, socket, finished](const asio::error_code &timerError)
		{
			if (timerError || *finished)
				return;

			*finished = true;

			asio::error_code closeError;
			socket->close(closeError);

			if (onError_)
				onError_("connect timeout");
		});

	socket->async_connect(
		endpoint,
		[this, self, socket, timeout, finished](const asio::error_code &error)
		{
			if (*finished)
				return;

			*finished = true;
			timeout->cancel();

			if (cancelled_)
			{
				asio::error_code closeError;
				socket->close(closeError);
				return;
			}

			if (error)
			{
				if (onError_)
					onError_(error.message());
				return;
			}

			if (!sendInitCode_)
			{
				if (onConnected_)
					onConnected_(std::move(*socket));
				return;
			}

			std::shared_ptr<std::uint32_t> initCode =
				std::make_shared<std::uint32_t>(static_cast<std::uint32_t>(INITCODE));

			asio::async_write(
				*socket, asio::buffer(initCode.get(), sizeof(std::uint32_t)),
				[this, self, socket, initCode](const asio::error_code &writeError,
				                               std::size_t bytes)
				{
					(void)bytes;

					if (cancelled_)
					{
						asio::error_code closeError;
						socket->close(closeError);
						return;
					}

					if (writeError)
					{
						if (onError_)
							onError_(writeError.message());
						return;
					}

					if (onConnected_)
						onConnected_(std::move(*socket));
				});
		});
}

void Connector::Cancel()
{
	cancelled_ = true;
	resolver_.cancel();
}

} // namespace net
