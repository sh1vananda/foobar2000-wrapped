#include "stdafx.h"
#include <map>
#include <algorithm>
#include <vector>
#include "../../pfc/filetimetools.h"

namespace {
	// {6E2A7F5A-1F7C-4A1B-8A3D-2E5F60708090}
	static const GUID guid_wrapped_index = { 0x6e2a7f5a, 0x1f7c, 0x4a1b, { 0x8a, 0x3d, 0x2e, 0x5f, 0x60, 0x70, 0x80, 0x90 } };
	// {CD3A4B5C-2E6D-4F8E-A1B2-C3D4E5F60718}
	static const GUID guid_wrapped_mainmenu = { 0xcd3a4b5c, 0x2e6d, 0x4f8e, { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18 } };

	static const char strWrappedPinTo[] = "%artist% - %title%";
	static const t_filetimestamp retentionPeriod = system_time_periods::week * 52; // Retain for a year

	struct wrapped_record_t {
		uint32_t play_count = 0;
		double total_time = 0;
		t_filetimestamp last_played = 0;
		pfc::string8 artist;
		pfc::string8 title;
	};

	static metadb_index_manager::ptr theAPI() {
		return metadb_index_manager::get();
	}

	class wrapped_index_client_impl : public metadb_index_client {
	public:
		wrapped_index_client_impl() {
			static_api_ptr_t<titleformat_compiler>()->compile_force(m_keyObj, strWrappedPinTo);
		}
		metadb_index_hash transform(const file_info& info, const playable_location& location) override {
			pfc::string_formatter str;
			m_keyObj->run_simple(location, &info, str);
			return metadb_index_client::from_md5(static_api_ptr_t<hasher_md5>()->process_single_string(str));
		}
	private:
		titleformat_object::ptr m_keyObj;
	};

	static wrapped_index_client_impl* get_client() {
		static service_impl_single_t<wrapped_index_client_impl> g_client;
		return &g_client;
	}

	static wrapped_record_t record_get(metadb_index_hash hash) {
		mem_block_container_impl temp;
		theAPI()->get_user_data(guid_wrapped_index, hash, temp);
		if (temp.get_size() > 0) {
			try {
				stream_reader_formatter_simple<false> reader(temp.get_ptr(), temp.get_size());
				wrapped_record_t ret;
				reader >> ret.play_count;
				reader >> ret.total_time;
				reader >> ret.last_played;
				reader >> ret.artist;
				reader >> ret.title;
				return ret;
			} catch (std::exception const&) {}
		}
		return wrapped_record_t();
	}

	static void record_set(metadb_index_hash hash, const wrapped_record_t& rec) {
		stream_writer_formatter_simple<false> writer;
		writer << rec.play_count;
		writer << rec.total_time;
		writer << rec.last_played;
		writer << rec.artist;
		writer << rec.title;
		theAPI()->set_user_data(guid_wrapped_index, hash, writer.m_buffer.get_ptr(), writer.m_buffer.get_size());
	}

	class wrapped_init : public init_stage_callback {
	public:
		void on_init_stage(t_uint32 stage) override {
			if (stage == init_stages::before_config_read) {
				try {
					theAPI()->add(get_client(), guid_wrapped_index, retentionPeriod);
				} catch (std::exception const& e) {
					FB2K_console_formatter() << "[Wrapped] Init failure: " << e;
				}
			}
		}
	};
	static service_factory_single_t<wrapped_init> g_wrapped_init;

	class wrapped_stats_collector : public playback_statistics_collector {
	public:
		void on_item_played(metadb_handle_ptr p_item) override {
			metadb_index_hash hash;
			if (get_client()->hashHandle(p_item, hash)) {
				wrapped_record_t rec = record_get(hash);
				rec.play_count++;
				rec.total_time += p_item->get_length();
				rec.last_played = filetimestamp_from_system_timer();
				
				// Ensure we have metadata even if not in library
				if (rec.artist.is_empty() || rec.title.is_empty()) {
					auto compiler = static_api_ptr_t<titleformat_compiler>();
					service_ptr_t<titleformat_object> fmtArtist, fmtTitle;
					compiler->compile_force(fmtArtist, "%artist%");
					compiler->compile_force(fmtTitle, "%title%");

					p_item->format_title(nullptr, rec.artist, fmtArtist, nullptr);
					p_item->format_title(nullptr, rec.title, fmtTitle, nullptr);
					
					if (rec.artist.is_empty()) rec.artist = "Unknown Artist";
					if (rec.title.is_empty()) rec.title = "Unknown Title";
				}

				record_set(hash, rec);
				theAPI()->dispatch_refresh(guid_wrapped_index, hash);

				FB2K_console_formatter() << "[Wrapped] Track played: " << rec.artist << " - " << rec.title << ". Total plays: " << rec.play_count;
			}
		}
	};
	static service_factory_single_t<wrapped_stats_collector> g_wrapped_stats_collector;

	static pfc::string8 format_duration(double seconds) {
		pfc::string_formatter out;
		int h = (int)(seconds / 3600);
		int m = (int)((seconds - h * 3600) / 60);
		int s = (int)(seconds - h * 3600 - m * 60);
		if (h > 0) out << h << "h ";
		if (m > 0 || h > 0) out << m << "m ";
		out << s << "s";
		return out.c_str();
	}

	class wrapped_mainmenu : public mainmenu_commands {
	public:
		t_uint32 get_command_count() override { return 1; }
		GUID get_command(t_uint32 index) override { return guid_wrapped_mainmenu; }
		void get_name(t_uint32 index, pfc::string_base& out) override { out = "Show My Wrapped Stats"; }
		bool get_description(t_uint32 index, pfc::string_base& out) override { out = "Shows a summary of your playback statistics."; return true; }
		GUID get_parent() override { return mainmenu_groups::view; }
		void execute(t_uint32 index, service_ptr_t<service_base> callback) override {
			pfc::list_t<metadb_index_hash> hashes;
			theAPI()->get_all_hashes(guid_wrapped_index, hashes);
			
			struct entry_t {
				metadb_index_hash hash;
				wrapped_record_t rec;
			};
			std::vector<entry_t> all_stats;
			double total_global_time = 0;
			uint32_t total_global_plays = 0;

			for (size_t i = 0; i < hashes.get_count(); ++i) {
				wrapped_record_t rec = record_get(hashes[i]);
				if (rec.play_count > 0) {
					all_stats.push_back({ hashes[i], rec });
					total_global_time += rec.total_time;
					total_global_plays += rec.play_count;
				}
			}

			std::sort(all_stats.begin(), all_stats.end(), [](const entry_t& a, const entry_t& b) {
				return a.rec.play_count > b.rec.play_count;
			});

			pfc::string_formatter report;
			report << "=== YOUR FOOBAR2000 WRAPPED ===\n\n";
			report << "Unique tracks logged: " << all_stats.size() << "\n";
			report << "Total play count: " << total_global_plays << "\n";
			report << "Total time listening: " << format_duration(total_global_time) << "\n\n";
			report << "--- TOP 10 TRACKS ---\n";

			size_t max_top = (std::min)(all_stats.size(), (size_t)10);
			for (size_t i = 0; i < max_top; ++i) {
				pfc::string8 name;
				name << all_stats[i].rec.artist << " - " << all_stats[i].rec.title;
				report << i + 1 << ". " << name << " (" << all_stats[i].rec.play_count << " plays)\n";
			}

			if (all_stats.size() == 0) {
				report << "(No playback data recorded yet. Play some tracks for at least 1 minute!)\n";
			}

			popup_message::g_show(report, "Wrapped Statistics");
		}
	};
	static service_factory_single_t<wrapped_mainmenu> g_wrapped_mainmenu;
}
