#include <clocale>
#include <cstdlib>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_shared.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

class ProcessLayout : public testing::TestWithParam<std::tuple<std::string, int, int, bool, int, int>> {};

TEST_P(ProcessLayout, sizes_and_positions_visible_panels_without_overlap) {
	const auto& [boxes, columns, percentage, proc_left, side_width, proc_width] = GetParam();

	ASSERT_EXIT({
		Term::width = columns;
		Term::height = 40;
		Config::set("color_theme", std::string{"Default"});
		Config::set("show_disks", false);
		Config::set("shown_boxes", boxes);
		Config::set("proc_left", proc_left);
		Config::set("proc_width_percent", percentage);
		Theme::setTheme();

		Draw::calcSizes();

		if (boxes.contains("mem")) {
			EXPECT_EQ(Mem::width, side_width);
			EXPECT_EQ(Mem::x, proc_left ? proc_width + 1 : 1);
		}
		if (boxes.contains("net")) {
			EXPECT_EQ(Net::width, side_width);
			EXPECT_EQ(Net::x, proc_left ? proc_width + 1 : 1);
		}
		if (boxes.contains("proc")) {
			EXPECT_EQ(Proc::width, proc_width);
			EXPECT_EQ(Proc::x, proc_left ? 1 : side_width + 1);
		}

		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(draw, ProcessLayout, testing::Values(
	std::tuple{"mem proc", 180, 75, false, 45, 135},
	std::tuple{"mem proc", 160, 75, false, 40, 120},
	std::tuple{"mem proc", 80, 75, false, 36, 44},
	std::tuple{"mem proc", 180, 10, false, 136, 44},
	std::tuple{"mem proc", 180, 99, false, 36, 144},
	std::tuple{"mem proc", 181, 75, false, 45, 136},
	std::tuple{"mem proc", 180, 75, true, 45, 135},
	std::tuple{"net proc", 180, 75, false, 45, 135},
	std::tuple{"mem net proc", 180, 75, false, 45, 135},
	std::tuple{"mem", 180, 75, false, 180, 0},
	std::tuple{"proc", 180, 75, false, 0, 180}
));

class ProcessDirectory : public testing::TestWithParam<bool> {};

TEST_P(ProcessDirectory, distinguishes_identical_commands_by_working_directory) {
	const bool tree = GetParam();

	ASSERT_EXIT({
		if (Config::strings.contains("shown_gpus")) Config::set("shown_gpus", std::string{});
		Config::set("show_disks", false);
		Shared::init();
		Term::width = 200;
		Term::height = 40;
		Config::set("shown_boxes", std::string{"proc"});
		Config::set("proc_tree", tree);
		Config::set("proc_cpu_graphs", false);
		Theme::setTheme();
		Draw::calcSizes();
		Proc::numpids = 2;
		std::vector<Proc::proc_info> processes(2);
		processes[0].pid = 42;
		processes[0].name = "worker";
		processes[0].cmd = "worker --mode=run";
		processes[0].cwd = "/srv/work/alpha";
		processes[1] = processes[0];
		processes[1].pid = 84;
		processes[1].cwd = "/srv/work/beta";
		processes[1].tree_index = 1;

		const auto output = Proc::draw(processes, true, true);

		EXPECT_NE(output.find("Cwd:"), std::string::npos);
		EXPECT_NE(output.find("/srv/work/alpha"), std::string::npos);
		EXPECT_NE(output.find("/srv/work/beta"), std::string::npos);
		EXPECT_NE(output.find("worker --mode=run"), std::string::npos);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(draw, ProcessDirectory, testing::Bool());

class DirectoryText : public testing::TestWithParam<std::pair<std::string, std::string>> {};

TEST_P(DirectoryText, displays_safe_working_directory_context) {
	const auto& [directory, expected_context] = GetParam();

	ASSERT_EXIT({
		ASSERT_NE(std::setlocale(LC_ALL, ""), nullptr);
		ASSERT_GT(MB_CUR_MAX, 1);
		if (Config::strings.contains("shown_gpus")) Config::set("shown_gpus", std::string{});
		Config::set("show_disks", false);
		Shared::init();
		Term::width = 135;
		Term::height = 40;
		Config::set("shown_boxes", std::string{"proc"});
		Config::set("proc_cpu_graphs", false);
		Theme::setTheme();
		Draw::calcSizes();
		Proc::numpids = 1;
		std::vector<Proc::proc_info> processes(1);
		processes[0].pid = 42;
		processes[0].name = "worker";
		processes[0].cmd = "worker";
		processes[0].cwd = directory;

		const auto output = Proc::draw(processes, true, true);

		EXPECT_NE(output.find(expected_context), std::string::npos);
		EXPECT_EQ(output.find(directory), std::string::npos);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(draw, DirectoryText, testing::Values(
	std::pair{"/srv/\x1b[31mproject\nalpha", "/srv/ [31mproject alpha"},
	std::pair{std::string{"/srv/"} + std::string(100, 'a') + "/项目", std::string{"/项目"}}
));

class ProcessText : public testing::TestWithParam<std::tuple<int, std::string, std::string, std::string, std::string>> {};

TEST_P(ProcessText, truncates_commands_and_directories_with_a_single_cell_ellipsis) {
	const auto& [width, command, directory, expected_command, expected_directory] = GetParam();

	ASSERT_EXIT({
		ASSERT_NE(std::setlocale(LC_ALL, ""), nullptr);
		ASSERT_GT(MB_CUR_MAX, 1);
		if (Config::strings.contains("shown_gpus")) Config::set("shown_gpus", std::string{});
		Config::set("show_disks", false);
		Shared::init();
		Term::width = 135;
		Term::height = 40;
		Config::set("shown_boxes", std::string{"proc"});
		Config::set("proc_cpu_graphs", false);
		Config::set("proc_column_pid", 8);
		Config::set("proc_column_command", width);
		Config::set("proc_column_cwd", width);
		Theme::setTheme();
		Draw::calcSizes();
		Proc::numpids = 1;
		std::vector<Proc::proc_info> processes(1);
		processes[0].pid = 1234567;
		processes[0].name = "worker";
		processes[0].cmd = command;
		processes[0].cwd = directory;

		const auto output = Proc::draw(processes, true, true);
		const auto body_start = output.find("1234567");
		ASSERT_NE(body_start, std::string::npos);
		std::string_view body{output};
		body.remove_prefix(body_start);

		EXPECT_NE(body.find(expected_command), std::string::npos);
		EXPECT_NE(body.find(expected_directory), std::string::npos);
		EXPECT_EQ(std::ranges::distance(std::views::split(body, std::string_view{"…"})), 3);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(draw, ProcessText, testing::Values(
	std::tuple{1, "abcdef", "/abcdef", "…", "…"},
	std::tuple{2, "abcdef", "/abcdef", "a…", "…f"},
	std::tuple{4, "abcdef", "/abcdef", "abc…", "…def"},
	std::tuple{4, "甲乙丙丁", "/甲乙丙丁", "甲…", "…丁"}
));
