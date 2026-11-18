#include "Session.h"

#include "FrameCodec.h"
#include "Protocol.h"

#include <atomic>
#include <cstring>

namespace net
{
namespace
{

std::atomic<std::uint64_t> g_nextGeneration{1};

constexpr std::size_t kDrainDeadlineMs = 1000;

std::string EndpointText(const asio::ip::tcp::endpoint &endpoint)
{
	std::string text = endpoint.address().to_string();
	text += ':';
	text += std::to_string(endpoint.port());
	return text;
}

} // namespace

Session::Session(asio::io_context &context, asio::ip::tcp::socket socket,
                 SessionMode mode, bool requireInit)
	: context_(context)
	, socket_(std::move(socket))
	, mode_(mode)
	, requireInit_(requireInit)
	, generation_(g_nextGeneration.fetch_add(1))
	, recv_(RECV_BUFFER_SIZE)
	, drainTimer_(context)
{
	initSeen_ = !requireInit_;
}

Session::~Session()
{
	asio::error_code ec;
	drainTimer_.cancel();
	if (socket_.is_open())
		socket_.close(ec);
}

void Session::Start()
{
	reading_ = true;
	open_ = true;
	DoRead();
}

void Session::SetFrameHandler(FrameHandler handler)
{
	onFrame_ = std::move(handler);
}

void Session::SetCloseHandler(CloseHandler handler)
{
	onClose_ = std::move(handler);
}

void Session::SetErrorHandler(ErrorHandler handler)
{
	onError_ = std::move(handler);
}

void Session::DoRead()
{
	if (!reading_ || !open_)
		return;

	if (writePos_ == recv_.size())
	{
		if (onError_)
			onError_("receive buffer full");

		ForceClose();
		return;
	}

	SessionPtr self = shared_from_this();

	socket_.async_read_some(
		asio::buffer(recv_.data() + writePos_, recv_.size() - writePos_),
		[this, self](const asio::error_code &error, std::size_t bytes)
		{
			OnRead(error, bytes);
		});
}

void Session::OnRead(const asio::error_code &error, std::size_t bytes)
{
	if (error)
	{
		if (closing_ || !open_)
			return;

		if (onError_)
			onError_(error.message().c_str());

		ForceClose();
		return;
	}

	if (bytes == 0)
	{
		ForceClose();
		return;
	}

	writePos_ += bytes;
	ProcessFrames();

	if (!open_ || !reading_)
		return;

	DoRead();
}

void Session::ProcessFrames()
{
	if (mode_ == SessionMode::Billing)
	{
		while (writePos_ - readPos_ >= BILLING_RECORD_SIZE)
		{
			if (onFrame_)
				onFrame_(recv_.data() + readPos_, BILLING_RECORD_SIZE);

			readPos_ += BILLING_RECORD_SIZE;

			if (!open_ || !reading_)
				break;
		}
	}
	else
	{
		while (open_ && reading_)
		{
			const DecodeOutcome outcome = DecodeNextFrame(
				recv_.data() + readPos_, writePos_ - readPos_,
				requireInit_, initSeen_);

			switch (outcome.status)
			{
			case DecodeStatus::NeedMoreData:
				readPos_ += outcome.consumed;
				goto compact;

			case DecodeStatus::InitMismatch:

				if (onError_)
					onError_("init code mismatch");

				ForceClose();
				return;

			case DecodeStatus::BadSize:
				if (onError_)
					onError_("frame size out of range");

				ForceClose();
				return;

			case DecodeStatus::BadChecksum:
				if (onError_)
					onError_("checksum mismatch");

				ForceClose();
				return;

			case DecodeStatus::Frame:
				readPos_ += outcome.consumed;

				if (onFrame_)
					onFrame_(outcome.frame, outcome.size);

				break;
			}
		}
	}

compact:

	if (readPos_ == 0)
		return;

	if (readPos_ >= writePos_)
	{
		readPos_ = 0;
		writePos_ = 0;
		return;
	}

	const std::size_t remaining = writePos_ - readPos_;
	std::memmove(recv_.data(), recv_.data() + readPos_, remaining);
	readPos_ = 0;
	writePos_ = remaining;
}

bool Session::Queue(const void *data, std::size_t size)
{
	if (!open_ || closing_ || size == 0)
		return false;

	if (queuedBytes_ + size > static_cast<std::size_t>(SEND_BUFFER_SIZE))
		return false;

	const std::uint8_t *bytes = static_cast<const std::uint8_t *>(data);
	queue_.emplace_back(bytes, bytes + size);
	queuedBytes_ += size;
	return true;
}

bool Session::Flush()
{
	if (!open_ || queue_.empty())
		return !queue_.empty();

	if (!writing_)
		StartWrite();

	return true;
}

bool Session::QueueAndFlush(const void *data, std::size_t size)
{
	if (!Queue(data, size))
		return false;

	return Flush();
}

void Session::StartWrite()
{
	if (!open_ || writing_ || queue_.empty())
		return;

	writing_ = true;
	SessionPtr self = shared_from_this();

	asio::async_write(
		socket_, asio::buffer(queue_.front()),
		[this, self](const asio::error_code &error, std::size_t bytes)
		{
			OnWrite(error, bytes);
		});
}

void Session::OnWrite(const asio::error_code &error, std::size_t bytes)
{
	writing_ = false;

	if (error)
	{
		if (onError_)
			onError_(error.message().c_str());

		ForceClose();
		return;
	}

	if (queue_.empty())
		return;

	queuedBytes_ -= bytes;
	queue_.pop_front();

	if (!queue_.empty())
	{
		StartWrite();
		return;
	}

	if (closing_)
		FinishClose();
}

void Session::Close()
{
	if (!open_ || closing_)
		return;

	closing_ = true;
	reading_ = false;

	asio::error_code ec;
	socket_.shutdown(asio::ip::tcp::socket::shutdown_receive, ec);

	if (writing_)
	{
		ArmDrainTimer();
		return;
	}

	if (queue_.empty())
	{
		FinishClose();
		return;
	}

	ArmDrainTimer();
	StartWrite();
}

void Session::ForceClose()
{
	if (closedNotified_)
		return;

	reading_ = false;
	writing_ = false;
	open_ = false;
	closing_ = false;
	queue_.clear();
	queuedBytes_ = 0;

	asio::error_code ec;
	drainTimer_.cancel();
	socket_.close(ec);

	NotifyClosed();
}

void Session::ArmDrainTimer()
{
	drainTimer_.expires_after(std::chrono::milliseconds(kDrainDeadlineMs));

	SessionPtr self = shared_from_this();

	drainTimer_.async_wait(
		[this, self](const asio::error_code &error)
		{
			if (!error)
				ForceClose();
		});
}

void Session::FinishClose()
{
	if (closedNotified_)
		return;

	reading_ = false;
	writing_ = false;
	open_ = false;
	closing_ = false;
	queue_.clear();
	queuedBytes_ = 0;

	asio::error_code ec;
	drainTimer_.cancel();
	socket_.close(ec);

	NotifyClosed();
}

void Session::NotifyClosed()
{
	if (closedNotified_)
		return;

	closedNotified_ = true;

	if (onClose_)
		onClose_();
}

std::string Session::PeerAddress() const
{
	asio::error_code ec;
	const asio::ip::tcp::endpoint endpoint = socket_.remote_endpoint(ec);
	return ec ? std::string() : EndpointText(endpoint);
}

} // namespace net




