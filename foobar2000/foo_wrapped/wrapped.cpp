#include "stdafx.h"
#include <map>
#include <algorithm>
#include <vector>
#include "../../pfc/filetimetools.h"
#include "resource.h"
#include <SDK/coreDarkMode.h>

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
		pfc::string8 album;
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
				if (reader.get_remaining() > 0) reader >> ret.album;
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
		writer << rec.album;
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
				
				auto compiler = static_api_ptr_t<titleformat_compiler>();
				service_ptr_t<titleformat_object> fmtArtist, fmtTitle, fmtAlbum;
				compiler->compile_force(fmtArtist, "%artist%");
				compiler->compile_force(fmtTitle, "%title%");
				compiler->compile_force(fmtAlbum, "%album%");

				p_item->format_title(nullptr, rec.artist, fmtArtist, nullptr);
				p_item->format_title(nullptr, rec.title, fmtTitle, nullptr);
				p_item->format_title(nullptr, rec.album, fmtAlbum, nullptr);
				
				if (rec.artist.is_empty()) rec.artist = "Unknown Artist";
				if (rec.title.is_empty()) rec.title = "Unknown Title";
				if (rec.album.is_empty()) rec.album = "Unknown Album";

				record_set(hash, rec);
				theAPI()->dispatch_refresh(guid_wrapped_index, hash);

				FB2K_console_formatter() << "[Wrapped] Track played: " << rec.artist << " - " << rec.title << " [" << rec.album << "]. Total plays: " << rec.play_count;
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

	struct stat_entry_t {
		pfc::string8 name;
		uint32_t plays;
		double time;
	};

	static void copy_to_clipboard(HWND wnd, const char* text) {
		if (OpenClipboard(wnd)) {
			EmptyClipboard();
			pfc::stringcvt::string_wide_from_utf8 wtext(text);
			size_t size = (wtext.length() + 1) * sizeof(wchar_t);
			HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
			if (hMem) {
				void* pMem = GlobalLock(hMem);
				if (pMem) {
					memcpy(pMem, wtext.get_ptr(), size);
					GlobalUnlock(hMem);
					SetClipboardData(CF_UNICODETEXT, hMem);
				}
			}
			CloseClipboard();
		}
	}

	struct DlgContext {
		const char* report;
		fb2k::CCoreDarkModeHooks hooks;
	};

	static INT_PTR CALLBACK WrappedDlgProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
		switch (msg) {
		case WM_INITDIALOG:
			{
				DlgContext* ctx = (DlgContext*)lp;
				SetProp(wnd, L"DLG_CONTEXT", (HANDLE)ctx);
				
				ctx->hooks.AddDialogWithControls(wnd);

				SetDlgItemTextW(wnd, IDC_REPORT_EDIT, pfc::stringcvt::string_wide_from_utf8(ctx->report).get_ptr());
			}
			return TRUE;
		case WM_COMMAND:
			switch (LOWORD(wp)) {
			case IDOK:
			case IDCANCEL:
				EndDialog(wnd, LOWORD(wp));
				break;
			case IDC_COPY_CLIPBOARD:
				{
					DlgContext* ctx = (DlgContext*)GetProp(wnd, L"DLG_CONTEXT");
					if (ctx && ctx->report) copy_to_clipboard(wnd, ctx->report);
				}
				break;
			}
			break;
		case WM_DESTROY:
			RemoveProp(wnd, L"DLG_CONTEXT");
			break;
		}
		return FALSE;
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
			
			std::vector<wrapped_record_t> all_records;
			std::map<pfc::string8, std::pair<uint32_t, double>> artist_stats;
			std::map<pfc::string8, std::pair<uint32_t, double>> album_stats;
			
			double total_global_time = 0;
			uint32_t total_global_plays = 0;

			for (size_t i = 0; i < hashes.get_count(); ++i) {
				wrapped_record_t rec = record_get(hashes[i]);
				if (rec.play_count > 0) {
					all_records.push_back(rec);
					total_global_time += rec.total_time;
					total_global_plays += rec.play_count;
					
					artist_stats[rec.artist].first += rec.play_count;
					artist_stats[rec.artist].second += rec.total_time;
					
					pfc::string8 album_key;
					album_key << rec.artist << " - " << rec.album;
					album_stats[album_key].first += rec.play_count;
					album_stats[album_key].second += rec.total_time;
				}
			}

			auto sort_stats = [](const std::map<pfc::string8, std::pair<uint32_t, double>>& src) {
				std::vector<stat_entry_t> v;
				for (auto const& [name, stats] : src) {
					v.push_back({ name, stats.first, stats.second });
				}
				std::sort(v.begin(), v.end(), [](const stat_entry_t& a, const stat_entry_t& b) {
					if (a.plays != b.plays) return a.plays > b.plays;
					return a.time > b.time;
				});
				return v;
			};

			std::sort(all_records.begin(), all_records.end(), [](const wrapped_record_t& a, const wrapped_record_t& b) {
				if (a.play_count != b.play_count) return a.play_count > b.play_count;
				return a.total_time > b.total_time;
			});

			auto top_artists = sort_stats(artist_stats);
			auto top_albums = sort_stats(album_stats);

			pfc::string_formatter report;
			report << "=== YOUR FOOBAR2000 WRAPPED ===\r\n\r\n";
			report << "Total play count: " << total_global_plays << "\r\n";
			report << "Total time listening: " << format_duration(total_global_time) << "\r\n\r\n";
			
			report << "--- TOP 5 ARTISTS ---\r\n";
			for (size_t i = 0; i < (std::min)(top_artists.size(), (size_t)5); ++i) {
				report << i + 1 << ". " << top_artists[i].name << " (" << top_artists[i].plays << " plays)\r\n";
			}
			report << "\r\n";

			report << "--- TOP 5 ALBUMS ---\r\n";
			for (size_t i = 0; i < (std::min)(top_albums.size(), (size_t)5); ++i) {
				report << i + 1 << ". " << top_albums[i].name << " (" << top_albums[i].plays << " plays)\r\n";
			}
			report << "\r\n";

			report << "--- TOP 10 TRACKS ---\r\n";
			for (size_t i = 0; i < (std::min)(all_records.size(), (size_t)10); ++i) {
				report << i + 1 << ". " << all_records[i].artist << " - " << all_records[i].title << " (" << all_records[i].play_count << " plays)\r\n";
			}

			if (all_records.size() == 0) {
				report << "(No playback data recorded yet. Play some tracks for at least 1 minute!)\r\n";
			}

			DlgContext ctx;
			ctx.report = report.c_str();
			DialogBoxParam(core_api::get_my_instance(), MAKEINTRESOURCE(IDD_WRAPPED_REPORT), core_api::get_main_window(), WrappedDlgProc, (LPARAM)&ctx);
		}
	};
	static service_factory_single_t<wrapped_mainmenu> g_wrapped_mainmenu;
}
