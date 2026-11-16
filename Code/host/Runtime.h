#ifndef _HOST_RUNTIME_H_
#define _HOST_RUNTIME_H_

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host
{

// Headless process runtime: one io_context, re-arming periodic ticks, signal
// handling, the optional interactive console and a status snapshot.
class Runtime
{
public:
	using CommandHandler = std::function<void(const std::string &command)>;
	using TickHandler = std::function<void()>;

	Runtime();
	~Runtime();

	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	asio::io_context &Context() { return context_; }

	void ScheduleTick(std::chrono::milliseconds interval, TickHandler handler);
	void Post(std::function<void()> work);

	void Run();
	void Stop();

	void SetShutdownHandler(std::function<void()> handler);
	void RequestShutdown();
	bool IsShuttingDown() const { return shuttingDown_.load(); }

	void StartSignals();
	void StopSignals();

	// Starts a stdin reader only when stdin is a terminal. Commands are posted
	// to the event loop in submission order.
	bool StartConsole(CommandHandler onCommand);
	void StopConsole();

	void PublishStatus(const std::string &line);
	std::string StatusSnapshot() const;

private:
	struct Tick
	{
		std::shared_ptr<asio::steady_timer> timer;
		std::chrono::milliseconds interval{ 0 };
		TickHandler handler;
	};

	void ArmTick(const std::shared_ptr<Tick> &tick);
	void CancelTicks();

	asio::io_context context_;
	asio::signal_set signals_;
	std::vector<std::shared_ptr<Tick>> ticks_;

	std::function<void()> shutdownHandler_;
	std::atomic<bool> shuttingDown_{ false };

	std::atomic<bool> consoleRunning_{ false };
	std::thread consoleThread_;

	mutable std::mutex statusMutex_;
	std::string statusLine_;
};

} // namespace host

#endif
