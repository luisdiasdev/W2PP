#ifndef _NET_RETRY_LADDER_H_
#define _NET_RETRY_LADDER_H_

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace net
{

// Non-blocking reconnect policy: up to N attempts spaced by a fixed delay.
//
// The legacy DB reconnect blocked the event loop with Sleep(200) between two
// connect attempts. The ladder keeps the same attempt count and spacing but
// schedules the next attempt with a steady_timer, so the process keeps serving
// traffic between attempts. The owner performs the I/O and reports the outcome.
class RetryLadder : public std::enable_shared_from_this<RetryLadder>
{
public:
	using AttemptHandler = std::function<void(int attempt)>;
	using ExhaustedHandler = std::function<void()>;
	using SuccessHandler = std::function<void()>;

	RetryLadder(asio::io_context &context, int attempts,
	            std::chrono::milliseconds delay);
	~RetryLadder();

	RetryLadder(const RetryLadder &) = delete;
	RetryLadder &operator=(const RetryLadder &) = delete;

	void SetHandlers(AttemptHandler onAttempt, ExhaustedHandler onExhausted,
	                 SuccessHandler onSuccess = SuccessHandler());

	// Starts the ladder and fires the first attempt immediately.
	void Start();

	// Reports the outcome of the attempt named by the last AttemptHandler.
	void ReportFailure();
	void ReportSuccess();

	// Stops the ladder without invoking any callback.
	void Stop();

	bool IsActive() const { return active_; }
	int AttemptsMade() const { return attemptsMade_; }
	int MaxAttempts() const { return maxAttempts_; }

private:
	void BeginAttempt();
	void ScheduleNext();
	void CancelTimer();

	asio::io_context &context_;
	asio::steady_timer timer_;
	int maxAttempts_;
	std::chrono::milliseconds delay_;

	int attemptsMade_ = 0;
	bool active_ = false;
	std::uint64_t generation_ = 0;

	AttemptHandler onAttempt_;
	ExhaustedHandler onExhausted_;
	SuccessHandler onSuccess_;
};

using RetryLadderPtr = std::shared_ptr<RetryLadder>;

} // namespace net

#endif
