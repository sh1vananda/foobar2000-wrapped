#include "stdafx.h"
#include <map>
#include <algorithm>
#include <vector>
#include <fstream>
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

	struct stat_entry_t {
		pfc::string8 name;
		uint32_t plays;
		double time;
	};

	struct wrapped_report_data_t {
		std::vector<wrapped_record_t> top_tracks;
		std::vector<stat_entry_t> top_artists;
		std::vector<stat_entry_t> top_albums;
		uint32_t global_plays = 0;
		double global_time = 0;
		pfc::string8 text_report;
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

	static void copy_to_clipboard(HWND wnd, const char* text) {
		if (OpenClipboard(wnd)) {
			EmptyClipboard();
			pfc::stringcvt::string_wide_from_utf8 wtext(text);
			size_t size = (wtext.length() + 1) * sizeof(wchar_t);
			HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
			if (hMem) {
				void* pMem = (void*)GlobalLock(hMem);
				if (pMem) {
					memcpy(pMem, wtext.get_ptr(), size);
					GlobalUnlock(hMem);
					SetClipboardData(CF_UNICODETEXT, hMem);
				}
			}
			CloseClipboard();
		}
	}

	static pfc::string8 escape_html(const char* in) {
		pfc::string8 out;
		while (*in) {
			char c = *in;
			switch (c) {
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			case '\"': out += "&quot;"; break;
			case '\'': out += "&apos;"; break;
			default: out.add_byte(c); break;
			}
			in++;
		}
		return out;
	}

	static void generate_web_wrapped(const wrapped_report_data_t& data) {
		pfc::string_formatter html;
		// Ensure meta charset is the very first thing
		html << "<!DOCTYPE html>\n<html lang='en'>\n<head>\n";
		html << "<meta charset='UTF-8'>\n";
		html << "<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n";
		html << "<title>Your Foobar2000 Wrapped</title>\n";
		html << "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@400;700&family=Outfit:wght@300;400;700&display=swap' rel='stylesheet'>\n";
		html << "<style>\n";
		html << "body { background: #0a0a0b; color: #fff; font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; margin: 0; overflow-x: hidden; }\n";
		html << ".container { max-width: 800px; margin: 0 auto; padding: 40px 20px; }\n";
		html << "header { text-align: center; margin-bottom: 80px; animation: fadeInDown 1s ease; }\n";
		html << "h1 { font-family: 'Outfit', sans-serif; font-size: 3.5rem; font-weight: 700; margin: 0; background: linear-gradient(135deg, #fff 0%, #a0a0ff 100%); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }\n";
		html << ".hero-stats { display: flex; gap: 20px; margin-bottom: 60px; }\n";
		html << ".stat-card { flex: 1; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.1); padding: 30px; border-radius: 24px; backdrop-filter: blur(10px); }\n";
		html << ".stat-val { font-size: 2.5rem; font-weight: 700; color: #ced4ff; }\n";
		html << ".stat-label { font-size: 0.9rem; text-transform: uppercase; letter-spacing: 2px; opacity: 0.5; margin-top: 10px; }\n";
		html << "section { margin-bottom: 60px; animation: fadeInUp 1s ease; }\n";
		html << "h2 { font-family: 'Outfit', sans-serif; font-size: 1.8rem; margin-bottom: 30px; border-left: 4px solid #7c83ff; padding-left: 15px; }\n";
		html << ".list { display: flex; flex-direction: column; gap: 12px; }\n";
		html << ".item { display: flex; align-items: center; background: rgba(255,255,255,0.02); padding: 15px 25px; border-radius: 16px; transition: 0.3s; border: 1px solid transparent; }\n";
		html << ".item:hover { background: rgba(255,255,255,0.05); border-color: rgba(255,255,255,0.1); transform: scale(1.02); }\n";
		html << ".rank { font-size: 1.2rem; font-weight: 700; width: 40px; opacity: 0.3; }\n";
		html << ".info { flex: 1; }\n";
		html << ".name { font-size: 1.1rem; font-weight: 400; }\n";
		html << ".sub { font-size: 0.85rem; opacity: 0.5; }\n";
		html << ".count { font-weight: 700; color: #7c83ff; }\n";
		html << "@keyframes fadeInDown { from { opacity: 0; transform: translateY(-30px); } to { opacity: 1; transform: translateY(0); } }\n";
		html << "@keyframes fadeInUp { from { opacity: 0; transform: translateY(30px); } to { opacity: 1; transform: translateY(0); } }\n";
		html << "</style>\n</head>\n<body>\n";
		html << "<div class='container'>\n";
		html << "<header><h1>MY WRAPPED</h1><p style='opacity:0.5'>Playback statistics for 2026</p></header>\n";
		
		html << "<div class='hero-stats'>\n";
		html << "<div class='stat-card'><div class='stat-val'>" << data.global_plays << "</div><div class='stat-label'>Total Plays</div></div>\n";
		html << "<div class='stat-card'><div class='stat-val'>" << format_duration(data.global_time) << "</div><div class='stat-label'>Listening Time</div></div>\n";
		html << "</div>\n";

		html << "<section><h2>Top Artists</h2><div class='list'>\n";
		for (size_t i = 0; i < (std::min)(data.top_artists.size(), (size_t)5); ++i) {
			pfc::string8 name = data.top_artists[i].name;
			if (name.is_empty()) name = "Unknown Artist";
			html << "<div class='item'><div class='rank'>" << i + 1 << "</div><div class='info'><div class='name'>" << escape_html(name) << "</div></div><div class='count'>" << data.top_artists[i].plays << " plays</div></div>\n";
		}
		html << "</div></section>\n";

		html << "<section><h2>Top Albums</h2><div class='list'>\n";
		for (size_t i = 0; i < (std::min)(data.top_albums.size(), (size_t)5); ++i) {
			pfc::string8 name = data.top_albums[i].name;
			if (name.is_empty()) name = "Unknown Album";
			html << "<div class='item'><div class='rank'>" << i + 1 << "</div><div class='info'><div class='name'>" << escape_html(name) << "</div></div><div class='count'>" << data.top_albums[i].plays << " plays</div></div>\n";
		}
		html << "</div></section>\n";

		html << "<section><h2>Top Tracks</h2><div class='list'>\n";
		for (size_t i = 0; i < (std::min)(data.top_tracks.size(), (size_t)10); ++i) {
			auto const& rec = data.top_tracks[i];
			html << "<div class='item'><div class='rank'>" << i + 1 << "</div><div class='info'><div class='name'>" << escape_html(rec.title) << "</div><div class='sub'>" << escape_html(rec.artist) << " &bull; " << escape_html(rec.album) << "</div></div><div class='count'>" << rec.play_count << "</div></div>\n";
		}
		html << "</div></section>\n";

		html << "</div>\n</body>\n</html>";

		pfc::string8 path;
		if (!uGetTempPath(path)) path = "C:\\";
		path += "wrapped_report.html";

		pfc::stringcvt::string_wide_from_utf8 wpath(path);
		std::ofstream f(wpath.get_ptr(), std::ios::binary);
		if (f.is_open()) {
			static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
			f.write((const char*)bom, sizeof(bom));
			f.write(html.c_str(), html.length());
			f.close();
			uShellExecute(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	struct DlgContext {
		wrapped_report_data_t* data;
		fb2k::CCoreDarkModeHooks hooks;
	};

	static INT_PTR CALLBACK WrappedDlgProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
		switch (msg) {
		case WM_INITDIALOG:
			{
				DlgContext* ctx = (DlgContext*)lp;
				SetProp(wnd, L"DLG_CONTEXT", (HANDLE)ctx);
				ctx->hooks.AddDialogWithControls(wnd);
				SetDlgItemTextW(wnd, IDC_REPORT_EDIT, pfc::stringcvt::string_wide_from_utf8(ctx->data->text_report).get_ptr());
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
					if (ctx && ctx->data) copy_to_clipboard(wnd, ctx->data->text_report);
				}
				break;
			case IDC_VIEW_WEB:
				{
					DlgContext* ctx = (DlgContext*)GetProp(wnd, L"DLG_CONTEXT");
					if (ctx && ctx->data) generate_web_wrapped(*(ctx->data));
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
			
			wrapped_report_data_t report_data;
			std::map<pfc::string8, std::pair<uint32_t, double>> artist_stats;
			std::map<pfc::string8, std::pair<uint32_t, double>> album_stats;
			
			for (size_t i = 0; i < hashes.get_count(); ++i) {
				wrapped_record_t rec = record_get(hashes[i]);
				if (rec.play_count > 0) {
					report_data.top_tracks.push_back(rec);
					report_data.global_time += rec.total_time;
					report_data.global_plays += rec.play_count;
					
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

			std::sort(report_data.top_tracks.begin(), report_data.top_tracks.end(), [](const wrapped_record_t& a, const wrapped_record_t& b) {
				if (a.play_count != b.play_count) return a.play_count > b.play_count;
				return a.total_time > b.total_time;
			});

			report_data.top_artists = sort_stats(artist_stats);
			report_data.top_albums = sort_stats(album_stats);

			pfc::string_formatter report;
			report << "=== YOUR FOOBAR2000 WRAPPED ===\r\n\r\n";
			report << "Total play count: " << report_data.global_plays << "\r\n";
			report << "Total time listening: " << format_duration(report_data.global_time) << "\r\n\r\n";
			
			report << "--- TOP 5 ARTISTS ---\r\n";
			for (size_t i = 0; i < (std::min)(report_data.top_artists.size(), (size_t)5); ++i) {
				report << i + 1 << ". " << report_data.top_artists[i].name << " (" << report_data.top_artists[i].plays << " plays)\r\n";
			}
			report << "\r\n";

			report << "--- TOP 5 ALBUMS ---\r\n";
			for (size_t i = 0; i < (std::min)(report_data.top_albums.size(), (size_t)5); ++i) {
				report << i + 1 << ". " << report_data.top_albums[i].name << " (" << report_data.top_albums[i].plays << " plays)\r\n";
			}
			report << "\r\n";

			report << "--- TOP 10 TRACKS ---\r\n";
			for (size_t i = 0; i < (std::min)(report_data.top_tracks.size(), (size_t)10); ++i) {
				report << i + 1 << ". " << report_data.top_tracks[i].artist << " - " << report_data.top_tracks[i].title << " (" << report_data.top_tracks[i].play_count << " plays)\r\n";
			}

			if (report_data.top_tracks.size() == 0) {
				report << "(No playback data recorded yet. Play some tracks for at least 1 minute!)\r\n";
			}
			report_data.text_report = report;

			DlgContext ctx;
			ctx.data = &report_data;
			DialogBoxParam(core_api::get_my_instance(), MAKEINTRESOURCE(IDD_WRAPPED_REPORT), core_api::get_main_window(), WrappedDlgProc, (LPARAM)&ctx);
		}
	};
	static service_factory_single_t<wrapped_mainmenu> g_wrapped_mainmenu;
}
