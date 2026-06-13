#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/helpers/helpers.h>
#include <map>
#include <string>
#include <cmath>
#include <algorithm>
#include <sstream>

DECLARE_COMPONENT_VERSION(
    "foo_resume",
    "1.1.0-alpha2",
    "Remembers playback position per track and resumes from where you left off.\n\n"
    "To enable or disable: Preferences -> Advanced -> Tools -> foo_resume.\n\n"
    "Built by reda777"
);

VALIDATE_COMPONENT_FILENAME("foo_resume.dll");

// ─── GUIDs ──────────────────────────────────────────────────────

static const GUID guid_advconfig_branch =
{ 0xbf752eec, 0xbc2a, 0x4913, { 0xb7, 0x6, 0xb8, 0x1d, 0xcc, 0x60, 0xac, 0x27 } };

static const GUID guid_advconfig_enabled =
{ 0x63573e37, 0x2d26, 0x42e4, { 0xba, 0xa, 0xf6, 0xd8, 0x79, 0x2f, 0xf6, 0x74 } };

static const GUID guid_advconfig_min_time =
{ 0xb72bff8, 0xc035, 0x4ec3, { 0xba, 0xfd, 0x6d, 0x85, 0x41, 0x42, 0xfd, 0x2d } };

// ─── Preferences (Preferences -> Advanced -> Tools -> foo_resume) ─

static advconfig_branch_factory g_advconfig_branch(
    "foo_resume",
    guid_advconfig_branch,
    advconfig_branch::guid_branch_tools,
    0);

static advconfig_checkbox_factory g_advconfig_enabled(
    "Enable playback resume",
    guid_advconfig_enabled,
    guid_advconfig_branch,
    0,
    true);

static advconfig_integer_factory g_advconfig_min_time(
    "Only track files longer than this duration (seconds)",
    guid_advconfig_min_time,
    guid_advconfig_branch,
    0,
    300,
    0,
    86400);

// ─── Constants ──────────────────────────────────────────────────

static constexpr double   kMinResumeTime = 10.0;
static constexpr size_t   kMaxEntries    = 5000;
static constexpr uint64_t kDebounceMs    = 7000;
static constexpr double   kSeekTolerance = 0.05;

// ─── Data structures ────────────────────────────────────────────

struct track_key {
    std::string path;
    t_uint32 subsong{};

    bool operator<(const track_key& o) const {
        if (path != o.path) return path < o.path;
        return subsong < o.subsong;
    }
};

struct position_entry {
    double   position{};
    uint64_t last_used{};
};

// ─── State ──────────────────────────────────────────────────────

static std::map<track_key, position_entry> g_positions;
static uint64_t g_access_counter = 0;
static bool     g_loaded = false;
static bool     g_dirty = false;
static uint64_t g_last_save_tick = 0;
static std::string g_config_path;

static track_key g_current_key{};
static bool      g_has_current = false;
static double    g_current_pos = 0.0;
static double    g_current_length = 0.0;
static bool      g_pending_seek = false;
static double    g_expected_seek = 0.0;

// ─── Config path ────────────────────────────────────────────────

static void init_config_path() {
    if (!g_config_path.empty()) return;
    pfc::string8 path = core_api::get_profile_path();
    path.add_filename("foo_resume_positions.txt");
    g_config_path = path.c_str();
}

// ─── LRU pruning ────────────────────────────────────────────────

static void prune_if_needed() {
    if (g_positions.size() <= kMaxEntries) return;

    auto oldest = std::min_element(
        g_positions.begin(), g_positions.end(),
        [](const auto& a, const auto& b) {
            return a.second.last_used < b.second.last_used;
        });

    if (oldest != g_positions.end())
        g_positions.erase(oldest);
}

// ─── Load ───────────────────────────────────────────────────────

static void load_positions() {
    if (g_loaded) return;
    g_loaded = true;

    try {
        auto data = filesystem::g_readWholeFile(
            g_config_path.c_str(), 10 * 1024 * 1024, fb2k::noAbort);
        if (!data.is_valid()) return;

        std::string content((const char*)data->get_ptr(), data->size());
        std::istringstream stream(content);
        std::string line;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            size_t p1 = line.find('\t');
            if (p1 == std::string::npos) continue;
            size_t p2 = line.find('\t', p1 + 1);
            if (p2 == std::string::npos) continue;

            try {
                track_key key;
                key.path = line.substr(0, p1);
                key.subsong = static_cast<t_uint32>(
                    std::stoul(line.substr(p1 + 1, p2 - p1 - 1)));
                double pos = std::stod(line.substr(p2 + 1));
                g_positions[key] = { pos, ++g_access_counter };
            }
            catch (...) {}
        }
    }
    catch (...) {}
}

// ─── Save ───────────────────────────────────────────────────────

static void save_positions() {
    const double minTime = (double)g_advconfig_min_time.get();
    try {
        filesystem::get(g_config_path.c_str())->rewrite_file(
            g_config_path.c_str(), fb2k::noAbort, 5.0,
            [minTime](file::ptr f) {
                const char header[] = "# foo_resume v1\n";
                f->write(header, strlen(header), fb2k::noAbort);

                size_t count = 0;
                for (const auto& kv : g_positions) {
                    pfc::string_formatter line;
                    line << kv.first.path.c_str()
                        << "\t" << kv.first.subsong
                        << "\t" << pfc::format_float(kv.second.position, 0, 3)
                        << "\n";
                    f->write(line.c_str(), line.length(), fb2k::noAbort);
                    if (++count >= kMaxEntries) break;
                }
            });
    }
    catch (...) {
        FB2K_console_formatter() << "foo_resume: save failed";
    }
}

static void save_if_dirty(bool force = false) {
    if (!g_dirty) return;
    uint64_t now = GetTickCount64();
    if (!force && (now - g_last_save_tick < kDebounceMs)) return;
    save_positions();
    g_dirty = false;
    g_last_save_tick = now;
}

// ─── Seek on main thread ────────────────────────────────────────

class seek_callback : public main_thread_callback {
public:
    seek_callback(double v) : m_seek(v) {}

    void callback_run() override {
        static_api_ptr_t<playback_control> pc;
        if (pc->is_playing())
            pc->playback_seek(m_seek);
    }

private:
    double m_seek;
};

// ─── Playback callback ──────────────────────────────────────────

namespace {

    class resume_play_callback : public play_callback {
    public:
        void on_playback_new_track(metadb_handle_ptr p_track) override {
            if (!g_advconfig_enabled.get()) return;
            const double minTime = (double)g_advconfig_min_time.get();

            init_config_path();
            load_positions();

            if (g_has_current && !g_pending_seek) {
                bool completed = g_current_length > 0.0 &&
                    g_current_pos >= (g_current_length - 2.0);
                if (completed) {
                    g_positions.erase(g_current_key);
                    g_dirty = true;
                }
                else if (g_current_length > minTime &&
                    g_current_pos > kMinResumeTime) {
                    g_positions[g_current_key] = { g_current_pos, ++g_access_counter };
                    prune_if_needed();
                    g_dirty = true;
                }
            }

            const playable_location& loc = p_track->get_location();
            g_current_key.path = loc.get_path();
            g_current_key.subsong = loc.get_subsong_index();
            g_current_pos = 0.0;
            g_has_current = true;
            g_pending_seek = false;

            file_info_impl info;
            g_current_length = p_track->get_info(info) ? info.get_length() : 0.0;

            auto it = g_positions.find(g_current_key);
            if (it != g_positions.end()) {
                double seek_to = it->second.position;
                it->second.last_used = ++g_access_counter;
                g_pending_seek = true;
                g_expected_seek = seek_to;
                static_api_ptr_t<main_thread_callback_manager>()
                    ->add_callback(new service_impl_t<seek_callback>(seek_to));
            }

            save_if_dirty();
        }

        void on_playback_stop(play_control::t_stop_reason reason) override {
            if (!g_advconfig_enabled.get()) return;
            if (!g_has_current) return;
            const double minTime = (double)g_advconfig_min_time.get();

            if (reason == play_control::stop_reason_eof) {
                g_positions.erase(g_current_key);
                g_dirty = true;
            }
            else if (g_current_length > minTime &&
                g_current_pos > kMinResumeTime) {
                g_positions[g_current_key] = { g_current_pos, ++g_access_counter };
                g_dirty = true;
            }

            save_if_dirty();
            g_has_current = false;
            g_current_pos = 0.0;
            g_pending_seek = false;
        }

        void on_playback_time(double t) override {
            if (!g_has_current || g_pending_seek) return;
            g_current_pos = t;
        }

        void on_playback_seek(double t) override {
            if (!g_has_current) return;
            g_current_pos = t;
            if (g_pending_seek && std::abs(t - g_expected_seek) < kSeekTolerance)
                g_pending_seek = false;
        }

        void on_playback_starting(play_control::t_track_command, bool) override {}
        void on_playback_pause(bool) override {}
        void on_playback_edited(metadb_handle_ptr) override {}
        void on_playback_dynamic_info(const file_info&) override {}
        void on_playback_dynamic_info_track(const file_info&) override {}
        void on_volume_change(float) override {}
    };

    static resume_play_callback g_callback;

    // ─── Init / Quit ────────────────────────────────────────────────

    class resume_initquit : public initquit {
    public:
        void on_init() override {
            static_api_ptr_t<play_callback_manager>()->register_callback(
                &g_callback,
                play_callback::flag_on_playback_new_track |
                play_callback::flag_on_playback_stop |
                play_callback::flag_on_playback_time |
                play_callback::flag_on_playback_seek,
                false);
        }

        void on_quit() override {
            const double minTime = (double)g_advconfig_min_time.get();
            if (g_advconfig_enabled.get() &&
                g_has_current &&
                g_current_length > minTime &&
                g_current_pos > kMinResumeTime)
            {
                g_positions[g_current_key] = { g_current_pos, ++g_access_counter };
                g_dirty = true;
            }

            save_if_dirty(true);

            static_api_ptr_t<play_callback_manager>()
                ->unregister_callback(&g_callback);
        }
    };

    FB2K_SERVICE_FACTORY(resume_initquit);

}