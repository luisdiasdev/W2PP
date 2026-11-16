#include "BillingLink.h"

#include "Protocol.h"

#include <cstdint>
#include <utility>

namespace net
{

BillingLink::BillingLink(asio::io_context &context, Connection &connection)
	: context_(context)
	, connection_(connection)
{
}

BillingLink::~BillingLink()
{
	if (connector_)
		connector_->Cancel();

	connection_.Detach();
}

void BillingLink::SetHandlers(RecordHandler onRecord,
                              ConnectedHandler onConnected,
                              LostHandler onLost)
{
	onRecord_ = std::move(onRecord);
	onConnected_ = std::move(onConnected);
	onLost_ = std::move(onLost);
}

bool BillingLink::Connect(const std::string &host, unsigned short port,
                          const std::string &bindAddress)
{
	if (connected_ || connecting_ || host.empty())
		return false;

	host_ = host;
	port_ = port;
	bindAddress_ = bindAddress;
	retryCounter_ = 0;
	disabled_ = false;
	connecting_ = true;

	connector_ = std::make_shared<Connector>(context_);

	connector_->Connect(
		host_, port_, bindAddress_, false,
		[this](asio::ip::tcp::socket socket)
		{
			OpenSession(std::move(socket));
		},
		[this](const std::string &error)
		{
			connecting_ = false;
			connected_ = false;
			disabled_ = true;
			connection_.Detach();
			NotifyLost(error);
		});

	return true;
}

void BillingLink::OpenSession(asio::ip::tcp::socket socket)
{
	SessionPtr session = std::make_shared<Session>(
		context_, std::move(socket), SessionMode::Billing, false);

	connection_.Attach(session);
	connecting_ = false;
	connected_ = true;
	retryCounter_ = 0;
	disabled_ = false;

	session->SetFrameHandler(
		[this](std::uint8_t *frame, std::size_t size)
		{
			if (!connected_ || size != BILLING_RECORD_SIZE)
				return;

			if (onRecord_)
				onRecord_(reinterpret_cast<char *>(frame));
		});

	session->SetCloseHandler(
		[this]()
		{
			connected_ = false;
			connection_.Detach();
			retryCounter_ = kRetryTicks;
			NotifyLost("billing connection closed");
		});

	session->SetErrorHandler(
		[this](const char *reason)
		{
			connected_ = false;
			retryCounter_ = kRetryTicks;
			NotifyLost(reason != nullptr ? reason : "billing error");
		});

	session->Start();

	if (onConnected_)
		onConnected_();
}

void BillingLink::Disconnect()
{
	if (connection_.IsOpen())
		connection_.CloseSocket();
	else
		retryCounter_ = kRetryTicks;
}

bool BillingLink::Tick()
{
	if (connected_ || connecting_ || disabled_)
		return false;

	if (retryCounter_ <= 0)
		return false;

	--retryCounter_;

	if (retryCounter_ > 0)
		return false;

	if (!Connect(host_, port_, bindAddress_))
	{
		disabled_ = true;
		NotifyLost("billing reconnect failed");
		return false;
	}

	return true;
}

void BillingLink::ResetRetry()
{
	retryCounter_ = 0;
}

void BillingLink::NotifyLost(const std::string &reason)
{
	if (onLost_)
		onLost_(reason.c_str());
}

} // namespace net
