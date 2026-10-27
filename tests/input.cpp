#include <array>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "btop_input.hpp"

class MouseFrames : public testing::TestWithParam<std::tuple<std::string, std::string, int, int>> {};

TEST_P(MouseFrames, decodes_complete_left_button_and_wheel_events) {
	const auto& [frame, key, column, row] = GetParam();
	const auto event = Input::parse_mouse(frame);
	ASSERT_TRUE(event.has_value());
	EXPECT_EQ(event->key, key);
	EXPECT_EQ(event->position[0], column);
	EXPECT_EQ(event->position[1], row);
}

INSTANTIATE_TEST_SUITE_P(input, MouseFrames, testing::Values(
	std::tuple{"[<0;17;3M", "mouse_click", 17, 3},
	std::tuple{"[<32;25;5M", "mouse_drag", 25, 5},
	std::tuple{"[<0;25;5m", "mouse_release", 25, 5},
	std::tuple{"[<64;4;9M", "mouse_scroll_up", 4, 9},
	std::tuple{"[<65;4;9M", "mouse_scroll_down", 4, 9}
));

class InvalidMouseFrames : public testing::TestWithParam<std::string> {};

TEST_P(InvalidMouseFrames, rejects_partial_malformed_and_unsupported_events) {
	EXPECT_FALSE(Input::parse_mouse(GetParam()).has_value());
}

INSTANTIATE_TEST_SUITE_P(input, InvalidMouseFrames, testing::Values(
	"[<0;17;3", "[<0;17M", "[<0;0;3M", "[<0;-1;3M", "[<0;17;0M", "[<0;17;xM",
	"[<0;2147483648;3M", "[<0;17;3xM", "[<4;17;3M", "[<32;17;3m", "[<0;17;3M[<0;17;3m"
));

class InputChunks : public testing::TestWithParam<std::pair<std::vector<std::string>, std::vector<std::string>>> {};

TEST_P(InputChunks, preserves_keyboard_events_and_mouse_frame_order) {
	const auto& [chunks, expected] = GetParam();
	std::array<int, 2> descriptors;

	ASSERT_EXIT({
		ASSERT_EQ(pipe(descriptors.data()), 0);
		ASSERT_EQ(dup2(descriptors[0], STDIN_FILENO), STDIN_FILENO);
		ASSERT_EQ(close(descriptors[0]), 0);
		const int flags = fcntl(STDIN_FILENO, F_GETFL);
		ASSERT_GE(flags, 0);
		ASSERT_EQ(fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK), 0);
		ASSERT_EQ(sigemptyset(&Input::signal_mask), 0);
		std::vector<std::string> observed;
		for (const auto& chunk : chunks) {
			ASSERT_EQ(write(descriptors[1], chunk.data(), chunk.size()), static_cast<ssize_t>(chunk.size()));
			while (Input::poll(0)) {
				auto key = Input::get();
				if (not key.empty()) observed.push_back(std::move(key));
			}
		}
		EXPECT_EQ(observed, expected);
		ASSERT_EQ(close(descriptors[1]), 0);
		std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
	}, testing::ExitedWithCode(EXIT_SUCCESS), "");
}

INSTANTIATE_TEST_SUITE_P(input, InputChunks, testing::Values(
	std::pair{std::vector<std::string>{"\033[<0;25;5Me\033[<32;30;5M\033[<0;30;5m"}, std::vector<std::string>{"mouse_click", "e", "mouse_drag", "mouse_release"}},
	std::pair{std::vector<std::string>{"\033[<0;25;", "5Me\033[<32;30;5M\033[<0;30;5m"}, std::vector<std::string>{"mouse_click", "e", "mouse_drag", "mouse_release"}},
	std::pair{std::vector<std::string>{"\033[<32;e"}, std::vector<std::string>{"e"}},
	std::pair{std::vector<std::string>{"\033[<32;2147483648", "e"}, std::vector<std::string>{"e"}},
	std::pair{std::vector<std::string>{"\033[<32;\033[<0;25;5m"}, std::vector<std::string>{"mouse_release"}}
));
