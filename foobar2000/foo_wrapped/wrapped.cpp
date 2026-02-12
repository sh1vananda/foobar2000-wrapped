#include "stdafx.h"
#include <map>
#include <algorithm>
#include <vector>
#include <fstream>
#include "../../pfc/filetimetools.h"
#include "resource.h"
#include <SDK/coreDarkMode.h>

namespace {

// ============================================================================
// CONSTANTS AND CONFIGURATION
// ============================================================================

static const GUID guid_wrapped_index = { 0x6e2a7f5a, 0x1f7c, 0x4a1b, { 0x8a, 0x3d, 0x2e, 0x5f, 0x60, 0x70, 0x80, 0x90 } };
static const GUID guid_wrapped_mainmenu = { 0xcd3a4b5c, 0x2e6d, 0x4f8e, { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18 } };

static const char strWrappedPinTo[] = "%artist% - %title%";
static const t_filetimestamp retentionPeriod = system_time_periods::week * 52;

// ============================================================================
// DATA STRUCTURES
// ============================================================================

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

// ============================================================================
// METADB INDEX CLIENT
// ============================================================================

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

static metadb_index_manager::ptr theAPI() {
	return metadb_index_manager::get();
}

// ============================================================================
// STORAGE OPERATIONS
// ============================================================================

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

// ============================================================================
// INITIALIZATION
// ============================================================================

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

// ============================================================================
// PLAYBACK STATISTICS COLLECTION
// ============================================================================

class wrapped_stats_collector : public playback_statistics_collector {
public:
	void on_item_played(metadb_handle_ptr p_item) override {
		metadb_index_hash hash;
		if (!get_client()->hashHandle(p_item, hash)) return;
		
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
	}
};
static service_factory_single_t<wrapped_stats_collector> g_wrapped_stats_collector;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

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
	if (!OpenClipboard(wnd)) return;
	
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

// ============================================================================
// WEB REPORT GENERATION
// ============================================================================

static void generate_web_wrapped(const wrapped_report_data_t& data) {
	pfc::string_formatter html;
	
	html << "<!DOCTYPE html>\n<html lang='en'>\n<head>\n";
	html << "<meta charset='UTF-8'>\n";
	html << "<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n";
	html << "<title>foobar2000 wrapped</title>\n";
	html << "<link href='https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Oswald:wght@400;700&family=Space+Mono:wght@400;700&family=Inter:wght@200;400;900&display=swap' rel='stylesheet'>\n";
	html << "<style>\n";
	html << "* { margin: 0; padding: 0; box-sizing: border-box; }\n";
	html << "body { font-family: 'Inter', sans-serif; background: #000; color: #fff; overflow-x: hidden; }\n";
	html << ".container { max-width: 1400px; margin: 0 auto; padding: 40px 30px; }\n";
	html << ".header { margin-bottom: 50px; }\n";
	html << ".header h1 { font-family: 'Bebas Neue', sans-serif; font-size: clamp(3rem, 10vw, 8rem); font-weight: 400; letter-spacing: 0.05em; line-height: 0.9; margin-bottom: 10px; }\n";
	html << ".header .year { font-family: 'Space Mono', monospace; font-size: clamp(1rem, 3vw, 1.5rem); opacity: 0.4; border-left: 3px solid #fff; padding-left: 20px; }\n";
	html << ".section { margin-bottom: 60px; position: relative; }\n";
	html << ".section-title { font-family: 'Oswald', sans-serif; font-size: clamp(1.5rem, 4vw, 3rem); font-weight: 700; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 30px; position: relative; display: inline-block; }\n";
	html << ".section-title::after { content: ''; position: absolute; bottom: -8px; left: 0; width: 60px; height: 4px; background: #fff; }\n";
	html << ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 2px; background: #fff; border: 2px solid #fff; }\n";
	html << ".stat-box { background: #000; padding: 40px 30px; text-align: center; border: none; transition: all 0.3s ease; }\n";
	html << ".stat-box:hover { background: #fff; color: #000; }\n";
	html << ".stat-number { font-family: 'Bebas Neue', sans-serif; font-size: clamp(3rem, 8vw, 6rem); font-weight: 400; line-height: 1; margin-bottom: 12px; letter-spacing: 0.05em; }\n";
	html << ".stat-label { font-family: 'Space Mono', monospace; font-size: 0.9rem; text-transform: uppercase; letter-spacing: 2px; opacity: 0.5; }\n";
	html << ".top-list { border: 2px solid #fff; padding: 30px; }\n";
	html << ".highlight-item { padding: 35px 0; border-bottom: 2px solid #fff; margin-bottom: 25px; }\n";
	html << ".highlight-label { font-family: 'Space Mono', monospace; font-size: 0.8rem; text-transform: uppercase; letter-spacing: 2px; opacity: 0.4; margin-bottom: 15px; }\n";
	html << ".highlight-name { font-family: 'Bebas Neue', sans-serif; font-size: clamp(2.5rem, 6vw, 5rem); font-weight: 400; line-height: 1; margin-bottom: 12px; letter-spacing: 0.05em; }\n";
	html << ".highlight-stats { font-family: 'Space Mono', monospace; font-size: 1.1rem; opacity: 0.5; }\n";
	html << ".list-item { display: grid; grid-template-columns: 60px 1fr auto; gap: 20px; align-items: center; padding: 18px 0; border-bottom: 1px solid rgba(255, 255, 255, 0.1); transition: all 0.2s ease; }\n";
	html << ".list-item:last-child { border-bottom: none; }\n";
	html << ".list-item:hover { background: rgba(255, 255, 255, 0.02); padding-left: 15px; margin-left: -15px; padding-right: 15px; margin-right: -15px; }\n";
	html << ".item-rank { font-family: 'Oswald', sans-serif; font-size: 2rem; font-weight: 700; opacity: 0.2; }\n";
	html << ".item-info h3 { font-family: 'Oswald', sans-serif; font-size: 1.4rem; font-weight: 600; margin-bottom: 5px; }\n";
	html << ".item-info p { font-family: 'Space Mono', monospace; font-size: 0.85rem; opacity: 0.5; }\n";
	html << ".item-count { font-family: 'Oswald', sans-serif; font-size: 1.6rem; font-weight: 700; text-align: right; }\n";
	html << ".item-count span { display: block; font-family: 'Space Mono', monospace; font-size: 0.65rem; font-weight: 400; opacity: 0.4; margin-top: 3px; }\n";
	html << ".footer { text-align: center; padding: 40px 0 25px; margin-top: 60px; }\n";
	html << ".footer-logo { margin-bottom: 15px; }\n";
	html << ".footer-logo img { opacity: 0.6; transition: opacity 0.3s ease; }\n";
	html << ".footer-logo img:hover { opacity: 1; }\n";
	html << ".footer p { font-family: 'Space Mono', monospace; font-size: 0.85rem; opacity: 0.4; }\n";
	html << "@media (max-width: 768px) { .list-item { grid-template-columns: 40px 1fr auto; gap: 15px; } .item-rank { font-size: 1.5rem; } .item-info h3 { font-size: 1.1rem; } .item-count { font-size: 1.3rem; } }\n";
	html << "</style>\n</head>\n<body>\n";
	
	html << "<div class='container'>\n";
	html << "<header class='header'>\n<h1>foobar2000<br>wrapped</h1>\n<p class='year'>2026 playback stats</p>\n</header>\n";

	html << "<section class='section'>\n<div class='stats-grid'>\n";
	html << "<div class='stat-box'><div class='stat-number'>" << data.global_plays << "</div><div class='stat-label'>Plays</div></div>\n";
	html << "<div class='stat-box'><div class='stat-number'>" << format_duration(data.global_time) << "</div><div class='stat-label'>Hours</div></div>\n";
	html << "<div class='stat-box'><div class='stat-number'>" << data.top_artists.size() << "</div><div class='stat-label'>Artists</div></div>\n";
	html << "</div>\n</section>\n";

	html << "<section class='section'>\n<h2 class='section-title'>Top Artists</h2>\n<div class='top-list'>\n";
	if (data.top_artists.size() > 0) {
		pfc::string8 name = data.top_artists[0].name;
		if (name.is_empty()) name = "Unknown Artist";
		html << "<div class='highlight-item'>\n<div class='highlight-label'>01 &middot; Most Played</div>\n";
		html << "<div class='highlight-name'>" << escape_html(name) << "</div>\n";
		html << "<div class='highlight-stats'>" << data.top_artists[0].plays << " plays</div>\n</div>\n";
	}
	for (size_t i = 1; i < (std::min)(data.top_artists.size(), (size_t)10); ++i) {
		pfc::string8 name = data.top_artists[i].name;
		if (name.is_empty()) name = "Unknown Artist";
		html << "<div class='list-item'>\n<div class='item-rank'>";
		if (i < 9) html << "0";
		html << i + 1 << "</div>\n<div class='item-info'><h3>" << escape_html(name) << "</h3></div>\n";
		html << "<div class='item-count'>" << data.top_artists[i].plays << "\n<span>PLAYS</span></div>\n</div>\n";
	}
	html << "</div>\n</section>\n";

	html << "<section class='section'>\n<h2 class='section-title'>Top Albums</h2>\n<div class='top-list'>\n";
	if (data.top_albums.size() > 0) {
		pfc::string8 name = data.top_albums[0].name;
		if (name.is_empty()) name = "Unknown Album";
		html << "<div class='highlight-item'>\n<div class='highlight-label'>01 &middot; Most Played</div>\n";
		html << "<div class='highlight-name'>" << escape_html(name) << "</div>\n";
		html << "<div class='highlight-stats'>" << data.top_albums[0].plays << " plays</div>\n</div>\n";
	}
	for (size_t i = 1; i < (std::min)(data.top_albums.size(), (size_t)10); ++i) {
		pfc::string8 name = data.top_albums[i].name;
		if (name.is_empty()) name = "Unknown Album";
		html << "<div class='list-item'>\n<div class='item-rank'>";
		if (i < 9) html << "0";
		html << i + 1 << "</div>\n<div class='item-info'><h3>" << escape_html(name) << "</h3></div>\n";
		html << "<div class='item-count'>" << data.top_albums[i].plays << "\n<span>PLAYS</span></div>\n</div>\n";
	}
	html << "</div>\n</section>\n";

	html << "<section class='section'>\n<h2 class='section-title'>Top Tracks</h2>\n<div class='top-list'>\n";
	if (data.top_tracks.size() > 0) {
		auto const& rec = data.top_tracks[0];
		html << "<div class='highlight-item'>\n<div class='highlight-label'>01 &middot; Most Played</div>\n";
		html << "<div class='highlight-name'>" << escape_html(rec.title) << "</div>\n";
		html << "<div class='highlight-stats'>" << escape_html(rec.artist) << " &middot; " << escape_html(rec.album) << " &middot; " << rec.play_count << " plays</div>\n</div>\n";
	}
	for (size_t i = 1; i < (std::min)(data.top_tracks.size(), (size_t)10); ++i) {
		auto const& rec = data.top_tracks[i];
		html << "<div class='list-item'>\n<div class='item-rank'>";
		if (i < 9) html << "0";
		html << i + 1 << "</div>\n<div class='item-info'>\n<h3>" << escape_html(rec.title) << "</h3>\n";
		html << "<p>" << escape_html(rec.artist) << " &middot; " << escape_html(rec.album) << "</p>\n</div>\n";
		html << "<div class='item-count'>" << rec.play_count << "\n<span>PLAYS</span></div>\n</div>\n";
	}
	html << "</div>\n</section>\n";

	html << "<footer class='footer'>\n";
	html << "<div class='footer-logo'><a href='https://www.foobar2000.org/' title='foobar2000 homepage'>";
	html << "<img src='https://www.foobar2000.org/button.png' alt='foobar2000 audio player' width='88' height='31' /></a></div>\n";
	html << "<p>Generated by foobar2000 wrapped</p>\n</footer>\n";

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

// ============================================================================
// DIALOG IMPLEMENTATION
// ============================================================================

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

// ============================================================================
// REPORT GENERATION
// ============================================================================

static wrapped_report_data_t generate_report_data() {
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

	std::sort(report_data.top_tracks.begin(), report_data.top_tracks.end(), 
		[](const wrapped_record_t& a, const wrapped_record_t& b) {
			if (a.play_count != b.play_count) return a.play_count > b.play_count;
			return a.total_time > b.total_time;
		});

	report_data.top_artists = sort_stats(artist_stats);
	report_data.top_albums = sort_stats(album_stats);

	return report_data;
}

static pfc::string8 format_text_report(const wrapped_report_data_t& data) {
	pfc::string_formatter report;
	report << "=== YOUR FOOBAR2000 WRAPPED ===\r\n\r\n";
	report << "Total play count: " << data.global_plays << "\r\n";
	report << "Total time listening: " << format_duration(data.global_time) << "\r\n\r\n";
	
	report << "--- TOP 5 ARTISTS ---\r\n";
	for (size_t i = 0; i < (std::min)(data.top_artists.size(), (size_t)5); ++i) {
		report << i + 1 << ". " << data.top_artists[i].name << " (" << data.top_artists[i].plays << " plays)\r\n";
	}
	report << "\r\n";

	report << "--- TOP 5 ALBUMS ---\r\n";
	for (size_t i = 0; i < (std::min)(data.top_albums.size(), (size_t)5); ++i) {
		report << i + 1 << ". " << data.top_albums[i].name << " (" << data.top_albums[i].plays << " plays)\r\n";
	}
	report << "\r\n";

	report << "--- TOP 10 TRACKS ---\r\n";
	for (size_t i = 0; i < (std::min)(data.top_tracks.size(), (size_t)10); ++i) {
		report << i + 1 << ". " << data.top_tracks[i].artist << " - " << data.top_tracks[i].title << " (" << data.top_tracks[i].play_count << " plays)\r\n";
	}

	if (data.top_tracks.size() == 0) {
		report << "(No playback data recorded yet. Play some tracks!)\r\n";
	}
	
	return report.c_str();
}

// ============================================================================
// MAIN MENU COMMAND
// ============================================================================

class wrapped_mainmenu : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { 
		return 1; 
	}
	
	GUID get_command(t_uint32 index) override { 
		return guid_wrapped_mainmenu; 
	}
	
	void get_name(t_uint32 index, pfc::string_base& out) override { 
		out = "Wrapped Stats"; 
	}
	
	bool get_description(t_uint32 index, pfc::string_base& out) override { 
		out = "View your playback statistics summary."; 
		return true; 
	}
	
	GUID get_parent() override { 
		return mainmenu_groups::view; 
	}
	
	void execute(t_uint32 index, service_ptr_t<service_base> callback) override {
		wrapped_report_data_t report_data = generate_report_data();
		report_data.text_report = format_text_report(report_data);
		
		DlgContext ctx;
		ctx.data = &report_data;
		DialogBoxParam(core_api::get_my_instance(), MAKEINTRESOURCE(IDD_WRAPPED_REPORT), 
			core_api::get_main_window(), WrappedDlgProc, (LPARAM)&ctx);
	}
};

static service_factory_single_t<wrapped_mainmenu> g_wrapped_mainmenu;

}
