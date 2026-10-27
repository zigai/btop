#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>
#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_shared.hpp"

TEST(collect, working_directory_follows_a_live_process_directory_change) {
	ASSERT_EXIT({
		if (Config::strings.contains("shown_gpus")) Config::set("shown_gpus", std::string{});
		Config::set("show_disks", false);
		Shared::init();
		const auto pid = static_cast<size_t>(getpid());
		std::filesystem::current_path("/");

		const auto& processes = Proc::collect();
		const auto process = std::ranges::find(processes, pid, &Proc::proc_info::pid);
		ASSERT_NE(process, processes.end());
		EXPECT_EQ(process->cwd, "/");

		Config::set("show_detailed", true);
		Config::set("detailed_pid", static_cast<int>(pid));
		std::filesystem::current_path("/proc");
		const auto& updated_processes = Proc::collect();
		const auto updated_process = std::ranges::find(updated_processes, pid, &Proc::proc_info::pid);
		ASSERT_NE(updated_process, updated_processes.end());
		EXPECT_EQ(updated_process->cwd, "/proc");
		EXPECT_EQ(Proc::detailed.entry.cwd, "/proc");

		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}
