#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_shared.hpp"

class ProcessFilter : public testing::TestWithParam<std::pair<std::string, bool>> {};

TEST_P(ProcessFilter, matches_working_directory_and_existing_process_fields) {
	const Proc::proc_info process{
		.pid = 42,
		.name = "worker",
		.cmd = "worker --mode=run",
		.cwd = "/srv/work/alpha",
		.user = "operator",
	};
	const auto& [filter, matches] = GetParam();

	EXPECT_EQ(Proc::matches_filter(process, filter), matches);
}

INSTANTIATE_TEST_SUITE_P(shared, ProcessFilter, testing::Values(
	std::pair{"work/ALPHA", true},
	std::pair{"!/work/alpha$", true},
	std::pair{"beta", false},
	std::pair{"!/work/beta$", false},
	std::pair{"!(", false},
	std::pair{"worker", true},
	std::pair{"mode=run", true},
	std::pair{"operator", true},
	std::pair{"42", true}
));

class HomePaths : public testing::TestWithParam<std::tuple<std::string, std::string, std::string>> {};

TEST_P(HomePaths, abbreviates_only_the_home_directory_and_its_children) {
	const auto& [home, path, expected] = GetParam();

	ASSERT_EXIT({
		if (home.empty()) ASSERT_EQ(unsetenv("HOME"), 0);
		else ASSERT_EQ(setenv("HOME", home.c_str(), 1), 0);
		EXPECT_EQ(Proc::abbreviate_home(path), expected);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(shared, HomePaths, testing::Values(
	std::tuple{"/home/tester", "/home/tester", "~"},
	std::tuple{"/home/tester", "/home/tester/work/job", "~/work/job"},
	std::tuple{"/home/tester", "/home/tester-other/job", "/home/tester-other/job"},
	std::tuple{"/home/tester/", "/home/tester/job", "~/job"},
	std::tuple{"/", "/work/job", "~/work/job"},
	std::tuple{"", "/work/job", "/work/job"}
));

TEST(shared, filter_accepts_the_displayed_home_path) {
	ASSERT_EXIT({
		ASSERT_EQ(setenv("HOME", "/home/tester", 1), 0);
		Proc::proc_info process;
		process.cwd = "/home/tester/work/job";
		EXPECT_TRUE(Proc::matches_filter(process, "~/work/job"));
		EXPECT_TRUE(Proc::matches_filter(process, "!^~/work/job$"));
		EXPECT_TRUE(Proc::matches_filter(process, "/home/tester/work/job"));
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

class ColumnRequests : public testing::TestWithParam<std::tuple<Proc::Column, int, bool>> {};

TEST_P(ColumnRequests, honors_a_column_width_when_the_table_has_room) {
	const auto& [column, requested, tree] = GetParam();

	ASSERT_EXIT({
		Config::set(Proc::column_specs.at(static_cast<size_t>(column)).key, requested);
		const auto layout = Proc::column_layout(240, tree, true);
		const auto actual = std::ranges::find(layout, column, &Proc::ColumnLayout::id);
		ASSERT_NE(actual, layout.end());
		EXPECT_EQ(actual->width, requested);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(shared, ColumnRequests, testing::Values(
	std::tuple{Proc::Column::pid, 12, false},
	std::tuple{Proc::Column::program, 24, false},
	std::tuple{Proc::Column::command, 60, false},
	std::tuple{Proc::Column::cwd, 50, false},
	std::tuple{Proc::Column::threads, 8, false},
	std::tuple{Proc::Column::user, 14, false},
	std::tuple{Proc::Column::memory, 9, false},
	std::tuple{Proc::Column::cpu, 16, false},
	std::tuple{Proc::Column::cpu, 4, false},
	std::tuple{Proc::Column::tree, 80, true},
	std::tuple{Proc::Column::cwd, 35, true}
));

class ColumnBounds : public testing::TestWithParam<std::tuple<int, bool, bool>> {};

TEST_P(ColumnBounds, keeps_columns_inside_the_table_with_extreme_width_requests) {
	const auto& [width, tree, custom] = GetParam();

	ASSERT_EXIT({
		if (custom) {
			for (const auto& spec : Proc::column_specs) Config::set(spec.key, std::numeric_limits<int>::max());
		}
		const auto layout = Proc::column_layout(width, tree, true);
		ASSERT_FALSE(layout.empty());
		int previous_end = 0;
		for (const auto& column : layout) {
			EXPECT_GE(column.width, 1);
			EXPECT_GT(column.x, previous_end);
			EXPECT_LE(column.x + column.width, width - 2);
			previous_end = column.x + column.width - 1;
		}
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(shared, ColumnBounds, testing::Combine(
	testing::Values(44, 55, 56, 80, 135, 200), testing::Bool(), testing::Bool()
));

class DirectorySorting : public testing::TestWithParam<bool> {};

TEST_P(DirectorySorting, orders_processes_by_their_working_directory) {
	const bool reverse = GetParam();
	std::vector<Proc::proc_info> processes = {
		{.pid = 40, .cwd = "/srv/bravo"},
		{.pid = 10, .cwd = "/srv/delta"},
		{.pid = 30, .cwd = "/srv/alpha"},
		{.pid = 20, .cwd = "/srv/charlie"}
	};
	const std::vector<size_t> expected = reverse ? std::vector<size_t>{10, 20, 40, 30} : std::vector<size_t>{30, 40, 20, 10};

	Proc::proc_sorter(processes, "cwd", reverse);

	std::vector<size_t> actual;
	for (const auto& process : processes) actual.push_back(process.pid);
	EXPECT_EQ(actual, expected);
}

TEST_P(DirectorySorting, orders_sibling_processes_by_their_working_directory) {
	const bool reverse = GetParam();
	std::vector<Proc::proc_info> processes = {
		{.pid = 40, .cwd = "/srv/bravo"},
		{.pid = 10, .cwd = "/srv/delta"},
		{.pid = 30, .cwd = "/srv/alpha"},
		{.pid = 20, .cwd = "/srv/charlie"}
	};
	std::vector<Proc::tree_proc> siblings;
	for (auto& process : processes) siblings.push_back({process, {}});
	const std::vector<size_t> expected = reverse ? std::vector<size_t>{10, 20, 40, 30} : std::vector<size_t>{30, 40, 20, 10};
	int position = 0;

	Proc::tree_sort(siblings, "cwd", reverse, false, position, static_cast<int>(processes.size()));

	std::vector<size_t> actual;
	for (const auto& sibling : siblings) actual.push_back(sibling.entry.get().pid);
	EXPECT_EQ(actual, expected);
}

INSTANTIATE_TEST_SUITE_P(shared, DirectorySorting, testing::Bool());
