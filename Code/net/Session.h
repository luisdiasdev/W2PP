#ifndef _NET_SESSION_H_
#define _NET_SESSION_H_

#include <asio.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net
{

enum class SessionMode
{
	Framed,
	Billing
};

// One asynchronous TCP session.
//
// All callbacks run on the io_context thread. Reads use a fixed 128 KiB
// buffer that is compacted after each batch; writes use a FIFO queue with the
// legacy 128 KiB cap and one outstanding async_write.
class Session : public std::enable_shared_from_this<Session>
{
public:
	using FrameHandler = std::function<void(std::uint8_t *frame, std::size_t size)>;
	using CloseHandler = std::function<void()>;
	using ErrorHandler = std::function<void(const char *reason)>;

	Session(asio::io_context &context, asio::ip::tcp::socket socket,
	        SessionMode mode, bool requireInit);
	~Session();

	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	void Start();

	void SetFrameHandler(FrameHandler handler);
	void SetCloseHandler(CloseHandler handler);
	void SetErrorHandler(ErrorHandler handler);

	// Appends already-encoded bytes (framed) or a billing record to the queue.
	bool Queue(const void *data, std::size_t size);

	// Starts the write pump when the queue is non-empty.
	bool Flush();

	bool QueueAndFlush(const void *data, std::size_t size);

	// Graceful close: stop reading, flush the queue with a one second deadline,
	// then close. ForceClose closes immediately and discards the queue.
	void Close();
	void ForceClose();

	bool IsOpen() const { return open_; }
	bool IsClosing() const { return closing_; }
	std::size_t QueuedBytes() const { return queuedBytes_; }
	std::uint64_t Generation() const { return generation_; }

	std::string PeerAddress() const;
	asio::ip::tcp::socket &Socket() { return socket_; }

private:
	void DoRead();
	void OnRead(const asio::error_code &error, std::size_t bytes);
	void ProcessFrames();
	void StartWrite();
	void OnWrite(const asio::error_code &error, std::size_t bytes);
	void ArmDrainTimer();
	void FinishClose();
	void NotifyClosed();

	asio::io_context &context_;
	asio::ip::tcp::socket socket_;
	SessionMode mode_;
	bool requireInit_;
	bool initSeen_ = false;
	bool reading_ = false;
	bool writing_ = false;
	bool open_ = true;
	bool closing_ = false;
	bool closedNotified_ = false;
	std::uint64_t generation_;

	std::vector<std::uint8_t> recv_;
	std::size_t readPos_ = 0;
	std::size_t writePos_ = 0;

	std::deque<std::vector<std::uint8_t>> queue_;
	std::size_t queuedBytes_ = 0;

	asio::steady_timer drainTimer_;

	FrameHandler onFrame_;
	CloseHandler onClose_;
	ErrorHandler onError_;
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace net

#endif
