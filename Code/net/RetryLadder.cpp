#include "RetryLadder.h"

#include <utility>

namespace net
{

RetryLadder::RetryLadder(asio::io_context &context, int attempts,
                         std::chrono::milliseconds delay)
	: context_(context)
	, timer_(context)
	, maxAttempts_(attempts > 0 ? attempts : 1)
	, delay_(delay)
{
}

RetryLadder::~RetryLadder()
{
	Stop();
}

void RetryLadder::SetHandlers(AttemptHandler onAttempt,
                              ExhaustedHandler onExhausted,
                              SuccessHandler onSuccess)
{
	onAttempt_ = std::move(onAttempt);
	onExhausted_ = std::move(onExhausted);
	onSuccess_ = std::move(onSuccess);
}

void RetryLadder::Start()
{
	if (active_)
		return;

	active_ = true;
	attemptsMade_ = 0;
	BeginAttempt();
}

void RetryLadder::BeginAttempt()
{
	if (!active_)
		return;

	++attemptsMade_;

	if (onAttempt_)
		onAttempt_(attemptsMade_);
}

void RetryLadder::ReportFailure()
{
	if (!active_)
		return;

	if (attemptsMade_ >= maxAttempts_)
	{
		active_ = false;
		CancelTimer();

		if (onExhausted_)
			onExhausted_();

		return;
	}

	ScheduleNext();
}

void RetryLadder::ReportSuccess()
{
	if (!active_)
		return;

	active_ = false;
	CancelTimer();

	if (onSuccess_)
		onSuccess_();
}

void RetryLadder::Stop()
{
	active_ = false;
	CancelTimer();
}

void RetryLadder::ScheduleNext()
{
	const std::uint64_t generation = ++generation_;

	timer_.expires_after(delay_);

	RetryLadderPtr self = shared_from_this();

	timer_.async_wait(
		[this, self, generation](const asio::error_code &error)
		{
			if (error || !active_ || generation != generation_)
				return;

			BeginAttempt();
		});
}

void RetryLadder::CancelTimer()
{
	++generation_;

	timer_.cancel();
}

} // namespace net
