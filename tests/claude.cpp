// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include "btop_claude.hpp"

using namespace Claude;

namespace {
	std::string line(const std::string& id, const std::string& usage, const std::string& type = "assistant", const std::string& ts = "2026-10-29T02:07:06.486Z") {
		return R"({"parentUuid":"x","message":{"id":")" + id + R"(","content":[{"type":"text","text":"a \"usage\": {} }"}],"usage":)" + usage
			+ R"(},"type":")" + type + R"(","timestamp":")" + ts + R"("})";
	}
	constexpr int64_t ts = 1792980000;
}

TEST(claude, parses_top_level_usage) {
	const auto r = parse_line(line("msg_1", R"({"input_tokens":2,"cache_creation_input_tokens":3,"cache_read_input_tokens":40,"output_tokens":5,"server_tool_use":{"web_search_requests":0}})"));
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->id, "msg_1");
	EXPECT_EQ(r->time, 1793239626);
	EXPECT_EQ(r->tokens.input, 2u);
	EXPECT_EQ(r->tokens.output, 5u);
	EXPECT_EQ(r->tokens.cache_write, 3u);
	EXPECT_EQ(r->tokens.cache_read, 40u);
	EXPECT_EQ(r->tokens.fresh(), 10u);
}

TEST(claude, falls_back_to_iterations_when_top_level_is_zero) {
	const auto r = parse_line(line("msg_2", R"({"input_tokens":0,"output_tokens":0,"iterations":[{"input_tokens":2,"output_tokens":470,"cache_read_input_tokens":411802,"cache_creation_input_tokens":539},{"output_tokens":10}]})"));
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->tokens.output, 480u);
	EXPECT_EQ(r->tokens.cache_read, 411802u);
}

TEST(claude, top_level_wins_over_iterations) {
	const auto r = parse_line(line("msg_3", R"({"output_tokens":7,"iterations":[{"output_tokens":7}]})"));
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->tokens.output, 7u);
}

TEST(claude, ignores_everything_else) {
	EXPECT_FALSE(parse_line(line("msg", R"({"output_tokens":1})", "user")).has_value());
	EXPECT_FALSE(parse_line(line("", R"({"output_tokens":1})")).has_value());
	EXPECT_FALSE(parse_line(line("msg", "null")).has_value());
	EXPECT_FALSE(parse_line(line("msg", R"({"output_tokens":1})", "assistant", "yesterday")).has_value());
	EXPECT_FALSE(parse_line(R"({"type":"user","message":{"content":"the word \"usage\" and \"assistant\""}})").has_value());
	EXPECT_FALSE(parse_line(R"({"type":"assistant","message":{"usage":{"output)").has_value());
	EXPECT_FALSE(parse_line("").has_value());
	EXPECT_FALSE(parse_line(std::string(200000, '[') + "\"usage\" \"assistant\"").has_value());
}

TEST(claude, ledger_dedupes_and_keeps_largest) {
	Ledger ledger;
	EXPECT_TRUE(ledger.add({"a", ts, {.output = 5}}));
	EXPECT_FALSE(ledger.add({"a", ts + 1, {.output = 5}}));
	EXPECT_TRUE(ledger.add({"a", ts + 2, {.output = 9}}));
	EXPECT_FALSE(ledger.add({"a", ts + 3, {.output = 3}}));
	EXPECT_EQ(ledger.size(), 1u);
	EXPECT_EQ(ledger.total(ts - 1, ts + 10).output, 9u);
}

TEST(claude, ledger_prune_keeps_index_usable) {
	Ledger ledger;
	ledger.add({"old", ts - 100, {.output = 1}});
	ledger.add({"new", ts, {.output = 2}});
	ledger.prune(ts - 10);
	EXPECT_EQ(ledger.size(), 1u);
	EXPECT_TRUE(ledger.add({"new", ts, {.output = 4}}));
	EXPECT_EQ(ledger.size(), 1u);
	EXPECT_EQ(ledger.total(ts - 1000, ts + 1).output, 4u);
}

TEST(claude, total_respects_window) {
	Ledger ledger;
	ledger.add({"a", ts - 100, {.output = 1}});
	ledger.add({"b", ts - 10, {.output = 2}});
	ledger.add({"future", ts + 100, {.output = 4}});
	EXPECT_EQ(ledger.total(ts - 50, ts).output, 2u);
	EXPECT_EQ(ledger.total(ts - 500, ts).output, 3u);
}

TEST(claude, buckets_place_events_on_grid) {
	Ledger ledger;
	const int64_t now = 1000 * 60 + 30;
	ledger.add({"current", now - 5, {.output = 1}});
	ledger.add({"previous", now - 40, {.output = 2}});
	ledger.add({"oldest_shown", now - 150, {.input = 4, .cache_read = 1000}});
	ledger.add({"too_old", now - 100000, {.output = 8}});
	ledger.add({"future", now + 5, {.output = 16}});
	const auto b = ledger.buckets(60, 4, now);
	ASSERT_EQ(b.size(), 4u);
	EXPECT_EQ(b.at(3), 1);
	EXPECT_EQ(b.at(2), 2);
	EXPECT_EQ(b.at(1), 4);
	EXPECT_EQ(b.at(0), 0);
}

TEST(claude, buckets_handle_bad_sizes) {
	Ledger ledger;
	EXPECT_TRUE(ledger.buckets(60, 0, 1000).empty());
	EXPECT_TRUE(ledger.buckets(60, -3, 1000).empty());
	EXPECT_EQ(ledger.buckets(0, 2, 1000).size(), 2u);
}

TEST(claude, statusline_reads_limits_and_model) {
	std::string model;
	const auto limits = parse_statusline(R"({"model":{"id":"x","display_name":"Opus"},"cwd":"/","rate_limits":{"five_hour":{"used_percentage":23.5,"resets_at":1738425600},"seven_day":{"used_percentage":41.2,"resets_at":1738857600},"spend_limit":{"used_percentage":1}}})", &model);
	EXPECT_EQ(model, "Opus");
	ASSERT_TRUE(limits.five_hour.has_value());
	EXPECT_DOUBLE_EQ(limits.five_hour->used, 23.5);
	EXPECT_EQ(limits.five_hour->resets_at, 1738425600);
	ASSERT_TRUE(limits.seven_day.has_value());
	EXPECT_DOUBLE_EQ(limits.seven_day->used, 41.2);
}

TEST(claude, statusline_tolerates_missing_windows) {
	EXPECT_FALSE(parse_statusline(R"({"model":{"display_name":"Opus"}})").five_hour.has_value());
	EXPECT_FALSE(parse_statusline(R"({"rate_limits":null})").five_hour.has_value());
	EXPECT_FALSE(parse_statusline("garbage").seven_day.has_value());
	EXPECT_FALSE(parse_statusline("").seven_day.has_value());
	const auto partial = parse_statusline(R"({"rate_limits":{"seven_day":{"used_percentage":5,"resets_at":9}}})");
	EXPECT_FALSE(partial.five_hour.has_value());
	EXPECT_TRUE(partial.seven_day.has_value());
}

TEST(claude, limits_round_trip_and_merge) {
	const Limits older {Limit{10, 100}, Limit{20, 200}};
	const auto parsed = parse_limits(serialize_limits(older));
	EXPECT_DOUBLE_EQ(parsed.five_hour->used, 10);
	EXPECT_EQ(parsed.seven_day->resets_at, 200);
	const auto merged = merge_limits(older, Limits{std::nullopt, Limit{30, 300}});
	EXPECT_DOUBLE_EQ(merged.five_hour->used, 10);
	EXPECT_DOUBLE_EQ(merged.seven_day->used, 30);
	EXPECT_FALSE(parse_limits("nonsense").five_hour.has_value());
}

TEST(claude, humanize) {
	EXPECT_EQ(humanize(0), "0");
	EXPECT_EQ(humanize(999), "999");
	EXPECT_EQ(humanize(1000), "1.0K");
	EXPECT_EQ(humanize(32826), "32.8K");
	EXPECT_EQ(humanize(809488), "809K");
	EXPECT_EQ(humanize(3157734), "3.2M");
	EXPECT_EQ(humanize(2'500'000'000), "2.5B");
}

TEST(claude, duration) {
	EXPECT_EQ(duration(-5), "0m");
	EXPECT_EQ(duration(60), "1m");
	EXPECT_EQ(duration(61), "2m");
	EXPECT_EQ(duration(2 * 3600 + 13 * 60), "2h13m");
	EXPECT_EQ(duration(3 * 86400 + 4 * 3600), "3d4h");
}

TEST(claude, range_seconds) {
	EXPECT_EQ(range_seconds("5h"), 18000);
	EXPECT_EQ(range_seconds("30d"), 2592000);
	EXPECT_EQ(range_seconds("nope"), 86400);
}
