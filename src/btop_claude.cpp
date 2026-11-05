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

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

#include <pthread.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "btop_claude.hpp"
#include "btop_log.hpp"
#include "btop_shared.hpp"

namespace fs = std::filesystem;
using std::string;
using std::string_view;

namespace Claude {
	const std::vector<string> ranges = {"5h", "24h", "7d", "30d"};
	const std::vector<string> positions = {"bottom", "top"};

	int64_t range_seconds(string_view range) {
		if (range == "5h") return 5 * 3600;
		if (range == "7d") return 7 * 86400;
		if (range == "30d") return 30 * 86400;
		return 86400;
	}

	Tokens& Tokens::operator+=(const Tokens& o) {
		input += o.input;
		output += o.output;
		cache_write += o.cache_write;
		cache_read += o.cache_read;
		return *this;
	}

	namespace {
		//* Forward only JSON reader, transcript lines reach megabytes and only a few fields matter.
		//* Skipping is iterative so hostile nesting can't overflow the stack.
		class Reader {
			string_view s;
			size_t p = 0;

		public:
			explicit Reader(string_view s) : s(s) {}

			void ws() {
				while (p < s.size() and (s[p] == ' ' or s[p] == '\t' or s[p] == '\n' or s[p] == '\r')) ++p;
			}
			char peek() {
				ws();
				return p < s.size() ? s[p] : '\0';
			}
			bool eat(char c) {
				if (peek() != c) return false;
				++p;
				return true;
			}

			//* Raw string contents, escapes are left alone since no field read here contains any
			std::optional<string_view> str() {
				if (peek() != '"') return std::nullopt;
				const size_t start = ++p;
				while (p < s.size() and s[p] != '"') p += (s[p] == '\\') ? 2 : 1;
				if (p >= s.size()) return std::nullopt;
				return s.substr(start, p++ - start);
			}

			bool skip() {
				const char c = peek();
				if (c == '"') return str().has_value();
				if (c != '{' and c != '[') {
					const size_t start = p;
					while (p < s.size() and not (s[p] == ',' or s[p] == '}' or s[p] == ']' or s[p] == ' ' or s[p] == '\n' or s[p] == '\r' or s[p] == '\t')) ++p;
					return p > start;
				}
				size_t depth = 0;
				while (p < s.size()) {
					const char d = s[p];
					if (d == '"') {
						if (not str()) return false;
						continue;
					}
					if (d == '{' or d == '[') ++depth;
					else if ((d == '}' or d == ']') and --depth == 0) {
						++p;
						return true;
					}
					++p;
				}
				return false;
			}

			//* Non-numeric values count as absent
			std::optional<double> number() {
				const char c = peek();
				if (c != '-' and (c < '0' or c > '9')) {
					skip();
					return std::nullopt;
				}
				const size_t start = p;
				skip();
				const string copy(s.substr(start, p - start));
				char* end = nullptr;
				const double value = std::strtod(copy.c_str(), &end);
				if (end != copy.c_str() + copy.size()) return std::nullopt;
				return value;
			}

			uint64_t count() {
				const auto value = number();
				return (value and *value > 0) ? static_cast<uint64_t>(*value) : 0;
			}

			//* Calls fn(key) on each member; fn consumes the value and returns false on bad input
			template <typename F>
			bool object(F&& fn) {
				if (not eat('{')) return false;
				if (eat('}')) return true;
				while (true) {
					const auto key = str();
					if (not key or not eat(':') or not fn(*key)) return false;
					if (eat(',')) continue;
					return eat('}');
				}
			}

			template <typename F>
			bool array(F&& fn) {
				if (not eat('[')) return false;
				if (eat(']')) return true;
				while (true) {
					if (not fn()) return false;
					if (eat(',')) continue;
					return eat(']');
				}
			}
		};

		bool read_tokens(Reader& r, string_view key, Tokens& t) {
			if (key == "input_tokens") t.input = r.count();
			else if (key == "output_tokens") t.output = r.count();
			else if (key == "cache_creation_input_tokens") t.cache_write = r.count();
			else if (key == "cache_read_input_tokens") t.cache_read = r.count();
			else return r.skip();
			return true;
		}

		std::optional<int64_t> parse_time(string_view ts) {
			//? Always UTC ("2026-10-29T02:07:06.486Z") in transcripts
			if (ts.size() < 20 or ts.back() != 'Z' or ts[10] != 'T') return std::nullopt;
			const auto field = [&](size_t pos, size_t len) {
				int value = -1;
				const auto [ptr, ec] = std::from_chars(ts.data() + pos, ts.data() + pos + len, value);
				return (ec == std::errc{} and ptr == ts.data() + pos + len) ? value : -1;
			};
			std::tm tm{};
			tm.tm_year = field(0, 4) - 1900;
			tm.tm_mon = field(5, 2) - 1;
			tm.tm_mday = field(8, 2);
			tm.tm_hour = field(11, 2);
			tm.tm_min = field(14, 2);
			tm.tm_sec = field(17, 2);
			if (tm.tm_year < 0 or tm.tm_mon < 0 or tm.tm_mday < 0 or tm.tm_hour < 0 or tm.tm_min < 0 or tm.tm_sec < 0) return std::nullopt;
			return static_cast<int64_t>(timegm(&tm));
		}
	}

	std::optional<Record> parse_line(string_view line) {
		//? Nearly all bytes in a transcript are in lines without usage data
		if (line.find("\"usage\"") == string_view::npos or line.find("\"assistant\"") == string_view::npos) return std::nullopt;

		Record rec;
		Tokens summed;
		bool is_assistant = false, has_usage = false;
		std::optional<int64_t> time;
		Reader r(line);

		const bool ok = r.object([&](string_view key) {
			if (key == "type") {
				const auto v = r.str();
				is_assistant = v and *v == "assistant";
				return v.has_value();
			}
			if (key == "timestamp") {
				const auto v = r.str();
				if (v) time = parse_time(*v);
				return v.has_value();
			}
			if (key != "message") return r.skip();
			if (r.peek() != '{') return r.skip();
			return r.object([&](string_view mkey) {
				if (mkey == "id") {
					const auto v = r.str();
					if (v) rec.id = *v;
					return v.has_value();
				}
				if (mkey != "usage") return r.skip();
				if (r.peek() != '{') return r.skip();
				has_usage = true;
				return r.object([&](string_view ukey) {
					if (ukey != "iterations") return read_tokens(r, ukey, rec.tokens);
					if (r.peek() != '[') return r.skip();
					return r.array([&] {
						if (r.peek() != '{') return r.skip();
						Tokens it;
						if (not r.object([&](string_view ikey) { return read_tokens(r, ikey, it); })) return false;
						summed += it;
						return true;
					});
				});
			});
		});

		if (not ok or not is_assistant or not has_usage or not time or rec.id.empty()) return std::nullopt;
		rec.time = *time;
		//? Some messages only report usage per iteration and leave the top level at zero
		if (rec.tokens.all() == 0) rec.tokens = summed;
		return rec;
	}

	bool Ledger::add(const Record& record) {
		const auto found = index.find(record.id);
		if (found == index.end()) {
			index.emplace(record.id, events.size());
			events.push_back({record.time, record.tokens});
			return true;
		}
		//? Same message seen again, partial writes hold smaller counts than the final one
		auto& event = events.at(found->second);
		if (record.tokens.all() <= event.tokens.all()) return false;
		event.tokens = record.tokens;
		return true;
	}

	void Ledger::prune(int64_t before) {
		if (std::ranges::none_of(events, [&](const Event& e) { return e.time < before; })) return;
		std::vector<Event> kept;
		std::unordered_map<string, size_t> kept_index;
		for (const auto& [id, pos] : index) {
			if (events.at(pos).time < before) continue;
			kept_index.emplace(id, kept.size());
			kept.push_back(events.at(pos));
		}
		events = std::move(kept);
		index = std::move(kept_index);
	}

	Tokens Ledger::total(int64_t since, int64_t now) const {
		Tokens sum;
		for (const auto& e : events)
			if (e.time >= since and e.time <= now) sum += e.tokens;
		return sum;
	}

	std::vector<long long> Ledger::buckets(int64_t bucket_seconds, int count, int64_t now) const {
		std::vector<long long> out(std::max(count, 0), 0);
		if (count <= 0 or bucket_seconds <= 0) return out;
		const int64_t end = (now / bucket_seconds + 1) * bucket_seconds;
		for (const auto& e : events) {
			if (e.time > now or e.time >= end) continue;
			const int64_t back = (end - 1 - e.time) / bucket_seconds;
			if (back < count) out.at(count - 1 - back) += e.tokens.fresh();
		}
		return out;
	}

	Limits parse_statusline(string_view json, string* model) {
		Limits limits;
		Reader r(json);
		const auto window = [&](std::optional<Limit>& out) {
			if (r.peek() != '{') return r.skip();
			std::optional<double> used, resets;
			const bool ok = r.object([&](string_view key) {
				if (key == "used_percentage") used = r.number();
				else if (key == "resets_at") resets = r.number();
				else return r.skip();
				return true;
			});
			if (ok and used) out = Limit{std::clamp(*used, 0.0, 100.0), static_cast<int64_t>(resets.value_or(0))};
			return ok;
		};
		r.object([&](string_view key) {
			if (key == "model" and model != nullptr and r.peek() == '{') {
				return r.object([&](string_view mkey) {
					if (mkey != "display_name" or r.peek() != '"') return r.skip();
					const auto v = r.str();
					if (v) *model = *v;
					return v.has_value();
				});
			}
			if (key != "rate_limits" or r.peek() != '{') return r.skip();
			return r.object([&](string_view rkey) {
				if (rkey == "five_hour") return window(limits.five_hour);
				if (rkey == "seven_day") return window(limits.seven_day);
				return r.skip();
			});
		});
		return limits;
	}

	string serialize_limits(const Limits& limits) {
		string out;
		if (limits.five_hour) out += fmt::format("five_hour {} {}\n", limits.five_hour->used, limits.five_hour->resets_at);
		if (limits.seven_day) out += fmt::format("seven_day {} {}\n", limits.seven_day->used, limits.seven_day->resets_at);
		return out;
	}

	Limits parse_limits(string_view text) {
		Limits limits;
		std::istringstream in{string(text)};
		string name;
		double used;
		int64_t resets;
		while (in >> name >> used >> resets) {
			const Limit limit{std::clamp(used, 0.0, 100.0), resets};
			if (name == "five_hour") limits.five_hour = limit;
			else if (name == "seven_day") limits.seven_day = limit;
		}
		return limits;
	}

	Limits merge_limits(const Limits& older, const Limits& newer) {
		return {newer.five_hour ? newer.five_hour : older.five_hour, newer.seven_day ? newer.seven_day : older.seven_day};
	}

	string humanize(uint64_t value) {
		static constexpr std::array<std::pair<uint64_t, char>, 4> units {{{1'000'000'000'000, 'T'}, {1'000'000'000, 'B'}, {1'000'000, 'M'}, {1'000, 'K'}}};
		for (const auto& [size, unit] : units) {
			if (value < size) continue;
			const double scaled = static_cast<double>(value) / static_cast<double>(size);
			return scaled < 99.95 ? fmt::format("{:.1f}{}", scaled, unit) : fmt::format("{:.0f}{}", scaled, unit);
		}
		return fmt::format("{}", value);
	}

	string duration(int64_t seconds) {
		if (seconds < 0) seconds = 0;
		const int64_t minutes = (seconds + 59) / 60;
		if (minutes < 60) return fmt::format("{}m", minutes);
		if (minutes < 24 * 60) return fmt::format("{}h{:02}m", minutes / 60, minutes % 60);
		return fmt::format("{}d{}h", minutes / (24 * 60), minutes / 60 % 24);
	}

	namespace {
		constexpr int64_t retention = 30 * 86400 + 3600;
		constexpr auto scan_interval = std::chrono::seconds(5);
		constexpr string_view release_repo = "GPTmadeit/btop";

		fs::path home() {
			const char* value = std::getenv("HOME");
			return value != nullptr ? fs::path(value) : fs::path();
		}

		std::vector<fs::path> transcript_roots() {
			if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir != nullptr and *dir != '\0')
				return {fs::path(dir) / "projects"};
			if (home().empty()) return {};
			return {home() / ".claude" / "projects", home() / ".config" / "claude" / "projects"};
		}

		fs::path limits_file() {
			const char* cache = std::getenv("XDG_CACHE_HOME");
			const fs::path base = (cache != nullptr and *cache != '\0') ? fs::path(cache) : (home().empty() ? fs::path() : home() / ".cache");
			return base.empty() ? fs::path() : base / "btop" / "claude_limits";
		}

		Limits read_limits_file() {
			const auto path = limits_file();
			if (path.empty()) return {};
			std::ifstream file(path);
			if (not file.good()) return {};
			return parse_limits(string(std::istreambuf_iterator<char>(file), {}));
		}

		//* Leaked on purpose, the reader thread is still running at exit
		struct State {
			std::mutex mtx;
			Ledger ledger;
			Limits limits;
			std::unordered_map<string, uintmax_t> offsets;
			uint64_t revision = 0;
			bool loaded = false;
			bool found = false;
			std::atomic<bool> active {true};
		};
		State& state() {
			static State* s = new State;
			return *s;
		}

		//* Read lines appended since the last scan; an unterminated last line is still being written
		void read_new_lines(const fs::path& path, uintmax_t& offset, std::vector<Record>& out) {
			std::ifstream file(path, std::ios::binary);
			if (not file.good()) return;
			file.seekg(static_cast<std::streamoff>(offset));
			string line;
			while (std::getline(file, line)) {
				if (file.eof()) break;
				offset += line.size() + 1;
				if (auto record = parse_line(line)) out.push_back(std::move(*record));
			}
		}

		void scan_once(State& st, int64_t now) {
			std::error_code ec;
			bool found = false;
			for (const auto& root : transcript_roots()) {
				auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
				if (ec) { ec.clear(); continue; }
				for (const auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
					if (ec) { ec.clear(); break; }
					const auto& path = it->path();
					if (path.extension() != ".jsonl" or not it->is_regular_file(ec)) continue;
					found = true;
					const auto size = it->file_size(ec);
					if (ec) { ec.clear(); continue; }

					//? No need to open files that can't hold anything inside the retention window
					const auto mtime = it->last_write_time(ec);
					if (ec) { ec.clear(); continue; }
					const auto age = fs::file_time_type::clock::now() - mtime;
					if (std::chrono::duration_cast<std::chrono::seconds>(age).count() > retention) continue;

					uintmax_t offset = 0;
					{
						std::lock_guard lock(st.mtx);
						offset = st.offsets[path.string()];
					}
					if (offset > size) offset = 0;
					if (offset == size) continue;

					std::vector<Record> records;
					read_new_lines(path, offset, records);

					std::lock_guard lock(st.mtx);
					st.offsets[path.string()] = offset;
					for (const auto& record : records)
						if (st.ledger.add(record)) ++st.revision;
				}
			}
			const auto limits = read_limits_file();

			std::lock_guard lock(st.mtx);
			st.ledger.prune(now - retention);
			st.limits = limits;
			st.found = found;
			st.loaded = true;
		}

		void reader_thread() {
			//? Signals belong to the main and runner threads
			sigset_t all;
			sigfillset(&all);
			pthread_sigmask(SIG_BLOCK, &all, nullptr);
			auto& st = state();
			while (true) {
				if (st.active) {
					try {
						scan_once(st, static_cast<int64_t>(std::time(nullptr)));
					}
					catch (const std::exception& e) {
						Logger::error("Claude usage scan failed: {}", e.what());
					}
				}
				std::this_thread::sleep_for(scan_interval);
			}
		}
	}

	void start() {
		static std::once_flag once;
		std::call_once(once, [] { std::thread(reader_thread).detach(); });
	}

	void set_active(bool active) {
		state().active = active;
	}

	Snapshot snapshot(int64_t bucket_seconds, int count, int64_t range, int64_t now) {
		auto& st = state();
		std::lock_guard lock(st.mtx);
		Snapshot snap;
		snap.loaded = st.loaded;
		snap.found_transcripts = st.found;
		snap.revision = st.revision;
		snap.limits = st.limits;
		snap.buckets = st.ledger.buckets(bucket_seconds, count, now);
		snap.range_total = st.ledger.total(now - range, now);
		snap.last_10m = st.ledger.total(now - 600, now);
		snap.last_5h = st.ledger.total(now - 5 * 3600, now);
		snap.last_24h = st.ledger.total(now - 86400, now);
		snap.last_7d = st.ledger.total(now - 7 * 86400, now);
		snap.last_30d = st.ledger.total(now - 30 * 86400, now);
		return snap;
	}

	void statusline_main(std::istream& in) {
		const string input(std::istreambuf_iterator<char>(in), {});
		string model;
		const auto incoming = parse_statusline(input, &model);
		const auto limits = merge_limits(read_limits_file(), incoming);

		if (const auto path = limits_file(); not path.empty() and (incoming.five_hour or incoming.seven_day)) {
			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			const fs::path tmp = path.string() + "." + std::to_string(getpid());
			{
				std::ofstream file(tmp, std::ios::trunc);
				file << serialize_limits(limits);
			}
			fs::rename(tmp, path, ec);
			if (ec) fs::remove(tmp, ec);
		}

		const auto now = static_cast<int64_t>(std::time(nullptr));
		string line = model;
		const auto add = [&](string_view label, const std::optional<Limit>& limit) {
			if (not limit or limit->resets_at <= now) return;
			line += fmt::format("{}{} {:.0f}% ({})", line.empty() ? "" : " | ", label, limit->used, duration(limit->resets_at - now));
		};
		add("5h", limits.five_hour);
		add("7d", limits.seven_day);
		fmt::println("{}", line);
	}

	void report_main() {
		auto& st = state();
		const auto now = static_cast<int64_t>(std::time(nullptr));
		scan_once(st, now);
		const auto roots = transcript_roots();
		fmt::println("{:<6} {:>14} {:>14} {:>14} {:>16}", "window", "input", "output", "cache write", "cache read");
		for (const string& range : {"5h"s, "24h"s, "7d"s, "30d"s}) {
			const auto t = st.ledger.total(now - range_seconds(range), now);
			fmt::println("{:<6} {:>14} {:>14} {:>14} {:>16}", range, t.input, t.output, t.cache_write, t.cache_read);
		}
		fmt::println("\n{} messages from {}", st.ledger.size(), st.found ? "transcripts" : "nowhere, no transcripts found");
		const auto limits = st.limits;
		if (limits.five_hour or limits.seven_day) {
			if (limits.five_hour) fmt::println("5h limit: {:.0f}%", limits.five_hour->used);
			if (limits.seven_day) fmt::println("7d limit: {:.0f}%", limits.seven_day->used);
		}
		else fmt::println("Plan limits: not available, see README for the status line setup");
	}

	namespace {
		//* Run a program without a shell, returns exit code and optionally stdout
		int run(const std::vector<string>& args, string* output = nullptr) {
			std::vector<char*> argv;
			for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
			argv.push_back(nullptr);

			int fds[2] = {-1, -1};
			if (output != nullptr and pipe(fds) != 0) return -1;
			const pid_t pid = fork();
			if (pid < 0) return -1;
			if (pid == 0) {
				if (output != nullptr) {
					dup2(fds[1], STDOUT_FILENO);
					close(fds[0]);
					close(fds[1]);
				}
				execvp(argv[0], argv.data());
				_exit(127);
			}
			if (output != nullptr) {
				close(fds[1]);
				char buf[4096];
				ssize_t n = 0;
				while ((n = read(fds[0], buf, sizeof(buf))) > 0) output->append(buf, static_cast<size_t>(n));
				close(fds[0]);
			}
			int status = 0;
			if (waitpid(pid, &status, 0) < 0) return -1;
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}

		std::optional<string> latest_tag() {
			string body;
			const string url = fmt::format("https://api.github.com/repos/{}/releases/latest", release_repo);
			if (run({"curl", "-fsSL", "-H", "Accept: application/vnd.github+json", url}, &body) != 0) return std::nullopt;
			std::optional<string> tag;
			Reader r(body);
			r.object([&](string_view key) {
				if (key != "tag_name" or r.peek() != '"') return r.skip();
				const auto v = r.str();
				if (v) tag = string(*v);
				return v.has_value();
			});
			//? The tag ends up in a URL
			if (tag and not std::ranges::all_of(*tag, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) or c == '.' or c == '-' or c == '_'; }))
				return std::nullopt;
			return tag;
		}
	}

	int self_update_main(bool force) {
	#ifndef __linux__
		(void)force;
		fmt::println(std::cerr, "--self-update is only available on Linux, releases have no other builds.");
		return 1;
	#else
		std::error_code ec;
		const auto self = fs::read_symlink("/proc/self/exe", ec);
		if (ec) {
			fmt::println(std::cerr, "Can't locate this binary: {}", ec.message());
			return 1;
		}
		utsname info{};
		if (uname(&info) != 0) return 1;
		const string asset = fmt::format("cbtop-{}-linux", info.machine);

		const auto tag = latest_tag();
		if (not tag) {
			fmt::println(std::cerr, "Couldn't get the latest release from github.com/{} (is curl installed?)", release_repo);
			return 1;
		}
		if (*tag == "v" + Global::Version and not force) {
			fmt::println("Already on the latest release ({}).", *tag);
			return 0;
		}
		fmt::println("Updating {} -> {}", Global::Version, *tag);

		const string base = fmt::format("https://github.com/{}/releases/download/{}/", release_repo, *tag);
		string sums;
		if (run({"curl", "-fsSL", base + "SHA256SUMS"}, &sums) != 0) {
			fmt::println(std::cerr, "Couldn't download SHA256SUMS for {}.", *tag);
			return 1;
		}
		string expected;
		{
			std::istringstream in(sums);
			string hash, name;
			while (in >> hash >> name)
				if (name == asset or name == "*" + asset) expected = hash;
		}
		if (expected.empty()) {
			fmt::println(std::cerr, "Release {} has no build named {}.", *tag, asset);
			return 1;
		}

		//? Same directory as the binary so the final rename is atomic
		const fs::path tmp = self.string() + ".update." + std::to_string(getpid());
		const auto fail = [&](string_view message) {
			fmt::println(std::cerr, "{}", message);
			fs::remove(tmp, ec);
			return 1;
		};

		if (run({"curl", "-fsSL", "-o", tmp.string(), base + asset}) != 0)
			return fail(fmt::format("Couldn't download {} to {}.", asset, tmp.string()));

		string actual;
		if (run({"sha256sum", tmp.string()}, &actual) != 0 or not actual.starts_with(expected + " "))
			return fail("Checksum mismatch, keeping the current binary.");

		fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec);
		if (ec) return fail(fmt::format("Couldn't make the download executable: {}", ec.message()));

		string version_output;
		if (run({tmp.string(), "-v"}, &version_output) != 0)
			return fail("The downloaded binary doesn't run on this system, keeping the current one.");

		fs::rename(tmp, self, ec);
		if (ec) return fail(fmt::format("Couldn't replace {}: {}", self.string(), ec.message()));

		fmt::println("Updated {} to {}.", self.string(), *tag);
		return 0;
	#endif
	}
}
