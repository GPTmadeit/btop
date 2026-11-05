/* Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//* Claude Code usage: reads the local session transcripts, no network and no credentials involved
namespace Claude {
	extern const std::vector<std::string> ranges;
	extern const std::vector<std::string> positions;

	//* Seconds covered by a value from ranges, 24h if unknown
	int64_t range_seconds(std::string_view range);

	struct Tokens {
		uint64_t input{}, output{}, cache_write{}, cache_read{};

		//* Tokens actually generated or newly processed; cache reads would dwarf everything else
		uint64_t fresh() const { return input + output + cache_write; }
		uint64_t all() const { return fresh() + cache_read; }
		Tokens& operator+=(const Tokens& o);
	};

	struct Record {
		std::string id;
		int64_t time{};
		Tokens tokens;
	};

	//* Parse one transcript line, empty unless it's an assistant message with usage data
	std::optional<Record> parse_line(std::string_view line);

	//* Usage records deduplicated by message id, a message is written once per content block and
	//* again in any session resumed from it
	class Ledger {
		struct Event {
			int64_t time;
			Tokens tokens;
		};
		std::vector<Event> events;
		std::unordered_map<std::string, size_t> index;

	public:
		bool add(const Record& record);
		void prune(int64_t before);
		size_t size() const { return events.size(); }
		Tokens total(int64_t since, int64_t now) const;

		//* Fresh tokens per bucket with the newest bucket last. Buckets sit on a fixed time grid
		//* so bars don't change shape as time passes.
		std::vector<long long> buckets(int64_t bucket_seconds, int count, int64_t now) const;
	};

	struct Limit {
		double used{};
		int64_t resets_at{};
	};

	//* Plan limits, only known when `btop --claude-statusline` is set as Claude Code's status line
	struct Limits {
		std::optional<Limit> five_hour, seven_day;
	};

	//* Parse the JSON Claude Code passes to a status line command
	Limits parse_statusline(std::string_view json, std::string* model = nullptr);
	std::string serialize_limits(const Limits& limits);
	Limits parse_limits(std::string_view text);
	Limits merge_limits(const Limits& older, const Limits& newer);

	std::string humanize(uint64_t value);
	std::string duration(int64_t seconds);

	struct Snapshot {
		bool loaded{};
		bool found_transcripts{};
		uint64_t revision{};
		std::vector<long long> buckets;
		Tokens range_total, last_5h, last_24h, last_7d, last_30d, last_10m;
		Limits limits;
	};

	//* Start the background reader (once) and pause or resume it
	void start();
	void set_active(bool active);
	Snapshot snapshot(int64_t bucket_seconds, int count, int64_t range, int64_t now);

	//* `--claude-statusline`: store plan limits from stdin, print a status line, always succeeds
	void statusline_main(std::istream& in);

	//* `--claude-report`: read all transcripts and print the totals
	void report_main();

	//* `--self-update`: replace this binary with the latest GitHub release, returns exit code
	int self_update_main(bool force);
}
