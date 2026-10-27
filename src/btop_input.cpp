/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
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
#include <charconv>
#include <cstdlib>
#include <limits>
#include <optional>
#include <ranges>
#include <vector>
#include <thread>
#include <mutex>
#include <fmt/format.h>
#include <signal.h>
#include <sys/select.h>
#include <utility>
#include <cmath>

#include "btop_input.hpp"
#include "btop_tools.hpp"
#include "btop_config.hpp"
#include "btop_shared.hpp"
#include "btop_menu.hpp"
#include "btop_draw.hpp"

using namespace Tools;
using namespace std::literals; // for operator""s
namespace rng = std::ranges;

namespace Input {

	//* Map for translating key codes to readable values
	const std::unordered_map<string, string> Key_escapes = {
		{"\033",	"escape"},
		{"\x12",	"ctrl_r"},
		{"\n",		"enter"},
		{" ",		"space"},
		{"\x7f",	"backspace"},
		{"\x08",	"backspace"},
		{"[A", 		"up"},
		{"OA",		"up"},
		{"[B", 		"down"},
		{"OB",		"down"},
		{"[D", 		"left"},
		{"OD",		"left"},
		{"[C", 		"right"},
		{"OC",		"right"},
		{"[2~",		"insert"},
		{"[4h",		"insert"},
		{"[3~",		"delete"},
		{"[P",		"delete"},
		{"[H",		"home"},
		{"[1~",		"home"},
		{"[F",		"end"},
		{"[4~",		"end"},
		{"[5~",		"page_up"},
		{"[6~",		"page_down"},
		{"\t",		"tab"},
		{"[Z",		"shift_tab"},
		{"OP",		"f1"},
		{"OQ",		"f2"},
		{"OR",		"f3"},
		{"OS",		"f4"},
		{"[15~",	"f5"},
		{"[17~",	"f6"},
		{"[18~",	"f7"},
		{"[19~",	"f8"},
		{"[20~",	"f9"},
		{"[21~",	"f10"},
		{"[23~",	"f11"},
		{"[24~",	"f12"}
	};

	sigset_t signal_mask;
	std::atomic<bool> polling (false);
	array<int, 2> mouse_pos;
	std::unordered_map<string, Mouse_loc> mouse_mappings;
	bool dragging_scroll;

	namespace {
		struct ColumnDrag {
			Proc::Column left, right;
			int start_x, left_width, right_width, table_x, table_y, table_width;
			uint64_t generation;
			std::optional<Proc::Column> clicked;
			bool moved = false;
			bool changed = false;
		};

		std::optional<ColumnDrag> column_drag;
		deque<string> pending_inputs;
		string mouse_fragment;

		auto table_snapshot() -> Proc::TableLayout {
			const std::scoped_lock lock{Proc::columns_mutex};
			return Proc::table_layout;
		}

		void queue_input(string bytes) {
			if (not mouse_fragment.empty()) {
				mouse_fragment += bytes;
				bytes = std::move(mouse_fragment);
				mouse_fragment.clear();
			}
			bool after_mouse_frame = false;
			while (not bytes.empty()) {
				if (bytes.starts_with("\033[<")) {
					const auto end = bytes.find_first_of("Mm", 3);
					const auto invalid = bytes.find_first_not_of("0123456789;Mm", 3);
					if (invalid != string::npos and (end == string::npos or invalid < end)) {
						pending_inputs.push_back(bytes.substr(0, invalid));
						bytes.erase(0, invalid);
						after_mouse_frame = false;
						continue;
					}
					if (end != string::npos) {
						pending_inputs.push_back(bytes.substr(0, end + 1));
						bytes.erase(0, end + 1);
						after_mouse_frame = true;
						continue;
					}
					std::string_view fields{bytes.data() + 3, bytes.size() - 3};
					bool valid = true;
					int separators = 0;
					while (valid) {
						const auto separator = fields.find(';');
						const auto field = fields.substr(0, separator);
						if (field.empty()) valid = separator == std::string_view::npos;
						else {
							int value;
							const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
							valid = parsed.ec == std::errc{} and parsed.ptr == field.data() + field.size();
						}
						if (separator == std::string_view::npos) break;
						if (++separators > 2) valid = false;
						fields.remove_prefix(separator + 1);
					}
					if (valid) mouse_fragment = std::move(bytes);
					else pending_inputs.push_back(std::move(bytes));
					return;
				}
				const auto next_mouse = bytes.find("\033[<");
				if (next_mouse != string::npos) {
					pending_inputs.push_back(bytes.substr(0, next_mouse));
					bytes.erase(0, next_mouse);
					after_mouse_frame = false;
					continue;
				}
				if (bytes == "\033[" or (bytes == "\033" and after_mouse_frame)) mouse_fragment = std::move(bytes);
				else pending_inputs.push_back(std::move(bytes));
				return;
			}
		}

		bool handle_header_mouse(const std::string_view key) {
			if (not column_drag or not key.starts_with("mouse_")) return false;
			if (key != "mouse_drag" and key != "mouse_release") {
				column_drag->moved = true;
				return true;
			}
			const auto drag = *column_drag;
			const bool release = key == "mouse_release";
			const bool moved = drag.moved or mouse_pos[0] != drag.start_x or mouse_pos[1] != drag.table_y;
			if (release) column_drag.reset();
			else column_drag->moved = moved;
			atomic_wait(Runner::active);
			Config::unlock();
			const auto table = table_snapshot();
			if (not Proc::shown or table.generation != drag.generation or table.x != drag.table_x or table.header_y != drag.table_y or table.width != drag.table_width) {
				column_drag.reset();
				return true;
			}
			const auto left = rng::find(table.columns, drag.left, &Proc::ColumnLayout::id);
			if (left == table.columns.end() or left + 1 == table.columns.end() or (left + 1)->id != drag.right) {
				column_drag.reset();
				return true;
			}
			if (release and not moved) {
				if (drag.clicked) {
					const auto sorting = Proc::column_specs.at(static_cast<size_t>(*drag.clicked)).sorting;
					if (Config::getS("proc_sorting") == sorting) Config::flip("proc_reversed");
					else {
						Config::set("proc_sorting", string{sorting});
						Config::set("proc_reversed", false);
					}
					Config::set("update_following", true);
					Runner::run("proc", false, true);
				}
				return true;
			}
			const int total = drag.left_width + drag.right_width;
			const int left_width = static_cast<int>(std::clamp(static_cast<long long>(drag.left_width) + mouse_pos[0] - drag.start_x, 1LL, static_cast<long long>(total - 1)));
			const int right_width = total - left_width;
			if (not drag.changed and left_width == drag.left_width and right_width == drag.right_width) return true;
			const auto left_key = Proc::column_specs.at(static_cast<size_t>(drag.left)).key;
			const auto right_key = Proc::column_specs.at(static_cast<size_t>(drag.right)).key;
			if (Config::getI(left_key) != left_width or Config::getI(right_key) != right_width) {
				Config::set(left_key, left_width);
				Config::set(right_key, right_width);
				if (column_drag) column_drag->changed = true;
				Runner::run("proc", true, true);
			}
			return true;
		}
	}

	auto parse_mouse(std::string_view sgr_body) -> std::optional<MouseEvent> {
		if (not sgr_body.starts_with("[<") or sgr_body.size() < 3) return {};
		const char terminator = sgr_body.back();
		if (terminator != 'M' and terminator != 'm') return {};
		sgr_body.remove_prefix(2);
		sgr_body.remove_suffix(1);
		array<int, 3> fields{};
		for (size_t index = 0; index < fields.size(); ++index) {
			const auto separator = index + 1 == fields.size() ? sgr_body.size() : sgr_body.find(';');
			if (separator == std::string_view::npos or separator == 0) return {};
			const auto field = sgr_body.substr(0, separator);
			const auto parsed = std::from_chars(field.data(), field.data() + field.size(), fields[index]);
			if (parsed.ec != std::errc{} or parsed.ptr != field.data() + field.size()) return {};
			sgr_body.remove_prefix(separator + (index + 1 < fields.size() ? 1 : 0));
		}
		if (fields[1] <= 0 or fields[2] <= 0) return {};
		string key;
		if (fields[0] == 0) key = terminator == 'm' ? "mouse_release" : "mouse_click";
		else if (terminator == 'm') return {};
		else if (fields[0] == 32) key = "mouse_drag";
		else if (fields[0] == 64) key = "mouse_scroll_up";
		else if (fields[0] == 65) key = "mouse_scroll_down";
		else return {};
		return MouseEvent{std::move(key), {fields[1], fields[2]}};
	}

	deque<string> history(50, "");
	string old_filter;
	string input;

	bool poll(const uint64_t timeout) {
		atomic_lock lck(polling);
		if (not pending_inputs.empty()) {
			input = std::move(pending_inputs.front());
			pending_inputs.pop_front();
			return true;
		}
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		struct timespec wait;
		struct timespec *waitptr = nullptr;

		if(timeout != std::numeric_limits<uint64_t>::max()) {
			wait.tv_sec = timeout / 1000;
			wait.tv_nsec = (timeout % 1000) * 1000000;
			waitptr = &wait;
		}

		if(pselect(STDIN_FILENO + 1, &fds, nullptr, nullptr, waitptr, &signal_mask) > 0) {
			string bytes;
			char buf[1024];
			ssize_t count = 0;
			while((count = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
				bytes.append(std::string_view(buf, count));
			}
			queue_input(std::move(bytes));
			if (pending_inputs.empty()) return false;
			input = std::move(pending_inputs.front());
			pending_inputs.pop_front();
			return true;
		}

		return false;
	}

	string get() {
		string key = input;
		if (not key.empty()) {
			//? Remove escape code prefix if present
			if (key.length() > 1 and key.at(0) == Fx::e.at(0)) {
				key.erase(0, 1);
			}
			
			//? Detect if input is a mouse event.
			if (key.starts_with("[<")) {
				const auto event = parse_mouse(key);
				if (not event) {
					column_drag.reset();
					dragging_scroll = false;
					return "";
				}
				key = event->key;
				mouse_pos = event->position;
				if (key == "mouse_click" or Menu::active) {
					column_drag.reset();
					dragging_scroll = false;
				}
				if (Config::getB("proc_filtering")) return key == "mouse_click" ? key : "";
				if (not column_drag and not dragging_scroll and key == "mouse_click") {
					atomic_wait(Runner::active);
					const auto& [col, line] = mouse_pos;
					for (const auto& [mapped_key, pos] : (Menu::active ? Menu::mouse_mappings : mouse_mappings)) {
						if (col >= pos.col and col < pos.col + pos.width and line >= pos.line and line < pos.line + pos.height) {
							key = mapped_key;
							break;
						}
					}
				}
			}
			else if (auto it = Key_escapes.find(key); it != Key_escapes.end())
				key = it->second;
			else if (ulen(key) > 1)
				key.clear();

			if (not key.starts_with("mouse_")) {
				column_drag.reset();
				dragging_scroll = false;
			}
			if (not key.empty()) {
				history.push_back(key);
				history.pop_front();
			}
		}
		return key;
	}

	string wait() {
		while(not poll(std::numeric_limits<uint64_t>::max())) {}
		return get();
	}

	void interrupt() {
		kill(getpid(), SIGUSR1);
	}

	void clear() {
		// do not need it, actually
	}

	void process(const std::string_view key) {
		if (key.empty()) return;
		try {
			if (not key.starts_with("mouse_")) {
				column_drag.reset();
				dragging_scroll = false;
			}
			if (handle_header_mouse(key)) return;
			auto filtering = Config::getB("proc_filtering");
			auto vim_keys = Config::getB("vim_keys");
			auto help_key = (vim_keys ? "H" : "h");
			auto kill_key = (vim_keys ? "K" : "k");
			//? Global input actions
			if (not filtering) {
				bool keep_going = false;
				if (key == "q") {
					clean_quit(0);
				}
				else if (is_in(key, "escape", "m")) {
					Menu::show(Menu::Menus::Main);
					return;
				}
				else if (is_in(key, "f1", "?", help_key)) {
					Menu::show(Menu::Menus::Help);
					return;
				}
				else if (is_in(key, "f2", "o")) {
					Menu::show(Menu::Menus::Options);
					return;
				}
				else if (key.size() == 1 and isint(key)) {
					auto intKey = std::atoi(key.data());
				#ifdef GPU_SUPPORT
					static const array<string, 10> boxes = {"gpu5", "cpu", "mem", "net", "proc", "gpu0", "gpu1", "gpu2", "gpu3", "gpu4"};
					if ((intKey == 0 and Gpu::count < 5) or (intKey >= 5 and intKey - 4 > Gpu::count))
						return;
				#else
				static const array<string, 10> boxes = {"", "cpu", "mem", "net", "proc"};
					if (intKey == 0 or intKey > 4)
						return;
				#endif
					atomic_wait(Runner::active);

					if (not Config::toggle_box(boxes.at(intKey))) {
						Menu::show(Menu::Menus::SizeError);
						return;
					}
					Config::current_preset.reset();
					Draw::calcSizes();
					Draw::update_clock(true);
					Runner::run("all", false, true);
					return;
				}
				else if (is_in(key, "p", "P") and Config::getS("disable_presets") != "All") {
					if (Config::getS("disable_presets") == "Default" and Config::preset_list.size() <= 1) return;
					atomic_wait(Runner::active);
					const auto old_preset = Config::current_preset;
					const int first_preset = (Config::getS("disable_presets") == "Default") ? 1 : 0;
					if (Config::getS("disable_presets") == "Custom") Config::current_preset = 0;
					else if (Config::current_preset.has_value()) {
						if (key == "p") {
							if(++(*Config::current_preset) >= static_cast<int>(Config::preset_list.size())) Config::current_preset = first_preset;
						}
						else if (--(*Config::current_preset) < first_preset) Config::current_preset = Config::preset_list.size() - 1;
					}
					else Config::current_preset = (key == "p") ? first_preset : Config::preset_list.size() - 1;
					if (Config::current_preset == old_preset) return;
					if (not Config::apply_preset(Config::preset_list.at(Config::current_preset.value()))) {
						Menu::show(Menu::Menus::SizeError);
						Config::current_preset = old_preset;
						return;
					}
					Draw::calcSizes();
					Draw::update_clock(true);
					Runner::run("all", false, true);
					return;
				} else if (is_in(key, "ctrl_r")) {
					kill(getpid(), SIGUSR2);
					return;
				} else if (key == "mouse_release") {
					dragging_scroll = false;
				} else
					keep_going = true;

				if (not keep_going) return;
			}

			//? Input actions for proc box
			if (Proc::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;
				if (filtering) {
					if (key == "enter" or key == "down") {
						Config::set("proc_filter", Proc::filter.text);
						Config::set("proc_filtering", false);
						old_filter.clear();
						if(key == "down"){
							Config::unlock();
							Config::lock();
							process("down");
							return;
						}
					}
					else if (key == "escape" or key == "mouse_click") {
						Config::set("proc_filter", old_filter);
						Config::set("proc_filtering", false);
						old_filter.clear();
					}
					else if (Proc::filter.command(key)) {
						if (Config::getS("proc_filter") != Proc::filter.text)
							Config::set("proc_filter", Proc::filter.text);
					}
					else
						return;
				}
				else if (key == "left" or (vim_keys and key == "h")) {
					int cur_i = v_index(Proc::sort_vector, Config::getS("proc_sorting"));
					if (--cur_i < 0)
						cur_i = Proc::sort_vector.size() - 1;
					Config::set("proc_sorting", Proc::sort_vector.at(cur_i));
					Config::set("update_following", true);
					if (Config::getB("proc_tree")) no_update = false;
				}
				else if (key == "right" or (vim_keys and key == "l")) {
					int cur_i = v_index(Proc::sort_vector, Config::getS("proc_sorting"));
					if (std::cmp_greater(++cur_i, Proc::sort_vector.size() - 1))
						cur_i = 0;
					Config::set("proc_sorting", Proc::sort_vector.at(cur_i));
					Config::set("update_following", true);
					if (Config::getB("proc_tree")) no_update = false;
				}
				else if (is_in(key, "f", "/")) {
					Config::flip("proc_filtering");
					Proc::filter = Draw::TextEdit{Config::getS("proc_filter")};
					old_filter = Proc::filter.text;
				}
				else if (key == "e") {
					Config::flip("proc_tree");
					no_update = false;
					Config::set("update_following", true);
				}
				else if (key == "E" and Config::getB("proc_tree")) {
					atomic_wait(Runner::active);
					Proc::collapse_all = 1;
					no_update = false;
				}
				else if (is_in(key, "u")) {
					Config::flip("pause_proc_list");
				}
				else if (is_in(key, "F")) {
					if (Config::getI("proc_selected") != 0 and Config::getI("followed_pid") != Config::getI("selected_pid")) {
						Config::set("follow_process", true);
						Config::set("followed_pid", Config::getI("selected_pid"));
						Config::set("update_following", true);
					}
					else if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Config::getI("followed_pid") != Config::getI("detailed_pid")) {
						Config::set("follow_process", true);
						Config::set("followed_pid", Config::getI("detailed_pid"));
						Config::set("update_following", true);
					}
					else if (Config::getB("follow_process")) {
						Config::flip("follow_process");
						if (Config::getB("should_selection_return_to_followed"))
							Config::set("proc_selected", Config::getI("proc_followed"));
						else if (Config::getB("show_detailed") and Config::getI("followed_pid") == Config::getI("detailed_pid"))
							Config::set("restore_detailed_pid", Config::getI("detailed_pid"));
						Config::set("followed_pid", 0);
						Config::set("proc_followed", 0);
					}
				}
				else if (key == "r") {
					Config::flip("proc_reversed");
					Config::set("update_following", true);
				}
				else if (key == "c")
					Config::flip("proc_per_core");

				else if (key == "%")
					Config::flip("proc_mem_bytes");

				else if (key == "delete" and not Config::getS("proc_filter").empty())
					Config::set("proc_filter", ""s);

				else if (key.starts_with("mouse_")) {
					redraw = false;
					const auto& [col, line] = mouse_pos;
					const int y = (Config::getB("show_detailed") ? Proc::y + 8 : Proc::y);
					const int height = (Config::getB("show_detailed") ? Proc::height - 8 : Proc::height);
					const auto in_proc_box = col >= Proc::x + 1 and col < Proc::x + Proc::width and line >= y + 1 and line < y + height - 1;
					if (key == "mouse_click") {
						atomic_wait(Runner::active);
						const auto table = table_snapshot();
						if (line == table.header_y and col >= table.x + 1 and col < table.x + table.width - 2) {
							std::optional<Proc::Column> clicked;
							for (const auto& column : table.columns) {
								if (col >= table.x + column.x and col < table.x + column.x + column.width) {
									clicked = column.id;
									break;
								}
							}
							int nearest_distance = table.width;
							for (size_t index = 0; index + 1 < table.columns.size(); ++index) {
								const auto& left = table.columns[index];
								const auto& right = table.columns[index + 1];
								const int distance = std::abs(col - (table.x + left.x + left.width));
								if (distance < nearest_distance) {
									nearest_distance = distance;
									column_drag = ColumnDrag{left.id, right.id, col, left.width, right.width, table.x, table.header_y, table.width, table.generation, clicked};
								}
							}
							dragging_scroll = false;
							return;
						}
						if (in_proc_box) {
							if (col < Proc::x + Proc::width - 2) {
								const auto& current_selection = Config::getI("proc_selected");
								if (current_selection == line - y - 1) {
									redraw = true;
									if (Config::getB("proc_tree")) {
										const int x_pos = col - Proc::x;
										const int offset = Config::getI("selected_depth") * 3;
										if (x_pos > offset and x_pos < 4 + offset and not table.columns.empty() and
											col < table.x + table.columns.front().x + table.columns.front().width) {
											process("space");
											return;
										}
									}
									process("enter");
									return;
								}
								else if (Config::getB("proc_banner_shown") and line == y + height - 2)
									return;
								else if (current_selection == 0 or line - y - 1 == 0)
									redraw = true;

								if (Config::getB("follow_process") and not Config::getB("pause_proc_list")) {
									Config::flip("follow_process");
									Config::set("followed_pid", 0);
									Config::set("proc_followed", 0);
									redraw = true;
								}

								Config::set("proc_selected", line - y - 1);
							}
							else if (line == y + 1) {
								if (Proc::selection("page_up") == -1) return;
							}
							else if (line == y + height - 2) {
								if (Proc::selection("page_down") == -1) return;
							}
							else if (line == y + 2 + Proc::scroll_pos) {
								dragging_scroll = true;
							}
							else if (Proc::selection("mousey" + to_string(line - y - 2)) == -1)
								return;
						}
						else if (Config::getI("proc_selected") > 0){
							Config::set("proc_selected", 0);
							if (Config::getB("follow_process") and not Config::getB("pause_proc_list")) {
								Config::flip("follow_process");
								Config::set("followed_pid", 0);
								Config::set("proc_followed", 0);
							}
							redraw = true;
						}
					}
					else if (key.starts_with("mouse_scroll_") and in_proc_box) {
						goto proc_mouse_scroll;
					}
					else if (key == "mouse_drag" and dragging_scroll) {
						Proc::selection("mousey" + to_string(line - y - 2));
					}
					else
						keep_going = true;
				}
				else if (is_in(key, "enter", "info_enter")) {
					if (Config::getI("proc_selected") == 0 and not Config::getB("show_detailed")) {
						return;
					}
					else if (Config::getI("proc_selected") > 0 and Config::getI("detailed_pid") != Config::getI("selected_pid")) {
						Config::set("detailed_pid", Config::getI("selected_pid"));
						Config::set("proc_last_selected", Config::getI("proc_selected"));
						Config::set("proc_selected", 0);
						if (Config::getB("proc_follow_detailed")) {
							Config::set("follow_process", true);
							Config::set("followed_pid", Config::getI("selected_pid"));
						}
						Config::set("show_detailed", true);
					}
					else if (Config::getB("show_detailed")) {
						if (Config::getB("proc_follow_detailed")) {
							Config::set("restore_detailed_pid", Config::getI("detailed_pid"));
							if (Config::getB("follow_process") and Config::getI("followed_pid") == Config::getI("detailed_pid")) {
								Config::flip("follow_process");
								Config::set("followed_pid", 0);
								Config::set("proc_followed", 0);
							}
						}
						else if (Config::getI("proc_last_selected") > 0) Config::set("proc_selected", Config::getI("proc_last_selected"));
						Config::set("proc_last_selected", 0);
						Config::set("detailed_pid", 0);
						Config::set("show_detailed", false);
					}
					Config::set("update_following", true);
				}
				else if (is_in(key, "+", "-", "space", "C", "=") and Config::getB("proc_tree")) {
					const bool is_following_detailed = Config::getB("follow_process") and Config::getI("followed_pid") == Config::getI("detailed_pid");
					if (Config::getI("proc_selected") > 0 or is_following_detailed) {
						atomic_wait(Runner::active);
						auto& pid = is_following_detailed and Config::getI("proc_selected") == 0 ? Config::getI("followed_pid") : Config::getI("selected_pid");
						if (key == "+" or key == "space" or key == "=") Proc::expand = pid;
						if (key == "-" or key == "space") Proc::collapse = pid;
						if (key == "C")	Proc::toggle_children = pid;
						no_update = false;
					}
					else
						keep_going = true;
				}
				else if (is_in(key, "t", kill_key) and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
					if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
					Menu::show(Menu::Menus::SignalSend, (key == "t" ? SIGTERM : SIGKILL));
					return;
				}
				else if (key == "s" and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
					if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
					Menu::show(Menu::Menus::SignalChoose);
					return;
				}
				else if (key == "N" and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
				    if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
				    Menu::show(Menu::Menus::Renice);
				    return;
			    }
				else if (is_in(key, "up", "down", "page_up", "page_down", "home", "end") or (vim_keys and is_in(key, "j", "k", "g", "G"))) {
					proc_mouse_scroll:
					redraw = false;
					auto old_selected = Config::getI("proc_selected");
					auto new_selected = Proc::selection(key);
					if (new_selected == -1)
						return;
					else if (old_selected != new_selected and (old_selected == 0 or new_selected == 0))
						redraw = true;
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("proc", no_update, redraw);
					Runner::run("cpu", no_update, redraw);
					return;
				}
			}

			//? Input actions for cpu box
			if (Cpu::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;
				static uint64_t last_press = 0;

				if ((key == "+" or key == "=") and Config::getI("update_ms") <= 86399900) {
					int add = (Config::getI("update_ms") <= 86399000 and last_press >= time_ms() - 200
						and rng::all_of(Input::history, [](const auto& str){ return str == "+"; })
						? 1000 : 100);
					Config::set("update_ms", Config::getI("update_ms") + add);
					last_press = time_ms();
					redraw = true;
				}
				else if (key == "-" and Config::getI("update_ms") >= 200) {
					int sub = (Config::getI("update_ms") >= 2000 and last_press >= time_ms() - 200
						and rng::all_of(Input::history, [](const auto& str){ return str == "-"; })
						? 1000 : 100);
					Config::set("update_ms", Config::getI("update_ms") - sub);
					last_press = time_ms();
					redraw = true;
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("cpu", no_update, redraw);
					return;
				}
			}

			//? Input actions for mem box
			if (Mem::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;

				if (key == "i") {
					Config::flip("io_mode");
				}
				else if (key == "d") {
					Config::flip("show_disks");
					no_update = false;
					Draw::calcSizes();
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("mem", no_update, redraw);
					return;
				}
			}

			//? Input actions for net box
			if (Net::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;

				if (is_in(key, "b", "n")) {
					atomic_wait(Runner::active);
					int c_index = v_index(Net::interfaces, Net::selected_iface);
					if (c_index != (int)Net::interfaces.size()) {
						if (key == "b") {
							if (--c_index < 0) c_index = Net::interfaces.size() - 1;
						}
						else if (key == "n") {
							if (++c_index == (int)Net::interfaces.size()) c_index = 0;
						}
						Net::selected_iface = Net::interfaces.at(c_index);
						Net::rescale = true;
					}
				}
				else if (key == "y") {
					Config::flip("net_sync");
					Net::rescale = true;
				}
				else if (key == "a") {
					Config::flip("net_auto");
					Net::rescale = true;
				}
				else if (key == "z") {
					atomic_wait(Runner::active);
					auto& ndev = Net::current_net.at(Net::selected_iface);
					if (ndev.stat.at("download").offset + ndev.stat.at("upload").offset > 0) {
						ndev.stat.at("download").offset = 0;
						ndev.stat.at("upload").offset = 0;
					}
					else {
						ndev.stat.at("download").offset = ndev.stat.at("download").last + ndev.stat.at("download").rollover;
						ndev.stat.at("upload").offset = ndev.stat.at("upload").last + ndev.stat.at("upload").rollover;
					}
					no_update = false;
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("net", no_update, redraw);
					return;
				}
			}
		}

		catch (const std::exception& e) {
			throw std::runtime_error { fmt::format(R"(Input::process("{}"))", e.what()) };
		}
	}
}
