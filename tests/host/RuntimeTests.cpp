#include <catch2/catch_test_macros.hpp>

#include "../../Code/host/Runtime.h"

#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("runtime posts commands in submission order", "[runtime]")
{
	host::Runtime runtime;
	std::vector<int> order;

	runtime.Post([&]() { order.push_back(1); });
	runtime.Post([&]() { order.push_back(2); });
	runtime.Post([&]() { order.push_back(3); });
	runtime.Post([&]() { runtime.Stop(); });

	runtime.Run();

	REQUIRE(order.size() == 3);
	REQUIRE(order[0] == 1);
	REQUIRE(order[1] == 2);
	REQUIRE(order[2] == 3);
}

TEST_CASE("runtime ticks re-arm without overlapping", "[runtime]")
{
	host::Runtime runtime;
	int ticks = 0;
	bool insideHandler = false;
	bool overlapped = false;

	runtime.ScheduleTick(
		std::chrono::milliseconds(2),
		[&]()
		{
			if (insideHandler)
				overlapped = true;

			insideHandler = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			++ticks;
			insideHandler = false;

			if (ticks == 3)
				runtime.RequestShutdown();
		});

	runtime.Run();

	REQUIRE(ticks == 3);
	REQUIRE_FALSE(overlapped);
	REQUIRE(runtime.IsShuttingDown());
}

TEST_CASE("runtime shutdown handler runs once", "[runtime]")
{
	host::Runtime runtime;
	int shutdownCalls = 0;

	runtime.SetShutdownHandler([&]() { ++shutdownCalls; });

	runtime.ScheduleTick(
		std::chrono::milliseconds(2),
		[&]() { runtime.RequestShutdown(); });

	runtime.Run();

	REQUIRE(shutdownCalls == 1);
	REQUIRE(runtime.IsShuttingDown());
}

TEST_CASE("runtime status snapshot is available without a window", "[runtime]")
{
	host::Runtime runtime;

	runtime.PublishStatus("group 1 index 2 users 3");
	REQUIRE(runtime.StatusSnapshot() == "group 1 index 2 users 3");
}
