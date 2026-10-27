#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_shared.hpp"

class ProcWidthValidation : public testing::TestWithParam<std::pair<std::string, bool>> {};

TEST_P(ProcWidthValidation, accepts_only_percentages_between_one_and_ninety_nine) {
	const auto& [value, accepted] = GetParam();
	EXPECT_EQ(Config::intValid("proc_width_percent", value), accepted);
}

INSTANTIATE_TEST_SUITE_P(config, ProcWidthValidation, testing::Values(
	std::pair{"-1", false},
	std::pair{"0", false},
	std::pair{"1", true},
	std::pair{"75", true},
	std::pair{"99", true},
	std::pair{"100", false},
	std::pair{"invalid", false}
));

TEST(config, rejects_negative_process_column_widths) {
	for (const auto& spec : Proc::column_specs) EXPECT_FALSE(Config::intValid(spec.key, "-1"));
}

TEST(config, accepts_automatic_and_positive_process_column_widths) {
	for (const auto& spec : Proc::column_specs) {
		EXPECT_TRUE(Config::intValid(spec.key, "0"));
		EXPECT_TRUE(Config::intValid(spec.key, "1"));
		EXPECT_TRUE(Config::intValid(spec.key, "120"));
		EXPECT_TRUE(Config::intValid(spec.key, "2147483647"));
	}
}
