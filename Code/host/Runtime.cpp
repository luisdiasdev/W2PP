#include "Runtime.h"

#include "../platform/Diagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#ifdef _WIN32
#include <io.h>
#endif

namespace host
{

Runtime::Runtime()
	: context_()
	, signals_(context_)
{
}

Runtime::~Runtime()
{
	StopConsole();
	CancelTicks();
}

void Runtime::ArmTick(const std::shared_ptr<Tick> &tick)
{
	if (shuttingDown_.load())
		return;

	tick->timer->expires_after(tick->interval);

	std::shared_ptr<Tick> keepAlive = tick;

	tick->timer->async_wait(
		[this, keepAlive](const asio::error_code &error)
		{
			if (error || shuttingDown_.load())
				return;

			keepAlive->handler();

			if (!shuttingDown_.load())
				ArmTick(keepAlive);
		});
}

void Runtime::ScheduleTick(std::chrono::milliseconds interval, TickHandler handler)
{
	ScheduleTickWithId(interval, std::move(handler));
}

Runtime::TickId Runtime::ScheduleTickWithId(std::chrono::milliseconds interval,
                                           TickHandler handler)
{
	std::shared_ptr<Tick> tick = std::make_shared<Tick>();
	tick->timer = std::make_shared<asio::steady_timer>(context_);
	tick->interval = interval;
	tick->handler = std::move(handler);

	ticks_.push_back(tick);
	ArmTick(tick);

	return tick;
}

void Runtime::SetTickInterval(const TickId &id, std::chrono::milliseconds interval)
{
	if (!id)
		return;

	std::shared_ptr<Tick> tick = std::static_pointer_cast<Tick>(id);

	if (tick->interval == interval)
		return;

	tick->interval = interval;

	if (shuttingDown_.load())
		return;

	// Restart the wait so the new interval takes effect immediately, as
	// SetTimer did.
	tick->timer->cancel();
	ArmTick(tick);
}

void Runtime::Post(std::function<void()> work)
{
	asio::post(context_, std::move(work));
}

void Runtime::Run()
{
	context_.run();
}

void Runtime::Stop()
{
	CancelTicks();
	context_.stop();
}

void Runtime::SetShutdownHandler(std::function<void()> handler)
{
	shutdownHandler_ = std::move(handler);
}

void Runtime::RequestShutdown()
{
	if (shuttingDown_.exchange(true))
		return;

	CancelTicks();

	asio::post(
		context_,
		[this]()
		{
			if (shutdownHandler_)
				shutdownHandler_();

			context_.stop();
		});
}

void Runtime::CancelTicks()
{
	for (const std::shared_ptr<Tick> &tick : ticks_)
		tick->timer->cancel();

	ticks_.clear();
}

void Runtime::StartSignals()
{
	signals_.add(SIGINT);
	signals_.add(SIGTERM);

	signals_.async_wait(
		[this](const asio::error_code &error, int signal)
		{
			(void)signal;

			if (!error)
				RequestShutdown();
		});
}

void Runtime::StopSignals()
{
	signals_.cancel();
}

bool Runtime::StartConsole(CommandHandler onCommand)
{
	bool forceConsole = false;

	if (const char *force = std::getenv("W2PP_FORCE_CONSOLE"))
		forceConsole = force[0] != '\0' && force[0] != '0';

#ifdef _WIN32
	if (!forceConsole && !_isatty(_fileno(stdin)))
		return false;
#endif

	if (consoleRunning_.exchange(true))
		return true;

	consoleThread_ = std::thread(
		[this, onCommand]()
		{
			std::string line;

			while (consoleRunning_.load() && std::getline(std::cin, line))
			{
				std::string command = line;

				while (!command.empty() &&
				       (command.back() == '\r' || command.back() == '\n'))
					command.pop_back();

				if (command.empty())
					continue;

				asio::post(
					context_,
					[this, onCommand, command]()
					{
						if (onCommand)
							onCommand(command);
					});
			}
		});

	return true;
}

void Runtime::StopConsole()
{
	consoleRunning_.store(false);

	if (consoleThread_.joinable())
		consoleThread_.detach();
}

void Runtime::PublishStatus(const std::string &line)
{
	{
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusLine_ = line;
	}

	platform::PublishStatus(line.c_str());
}

std::string Runtime::StatusSnapshot() const
{
	std::lock_guard<std::mutex> lock(statusMutex_);
	return statusLine_;
}

} // namespace host
