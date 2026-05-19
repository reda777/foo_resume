#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/helpers/helpers.h>
#include <map>
#include <string>
#include <windows.h>
#include <cmath>
#include <memory>
#include <algorithm>

DECLARE_COMPONENT_VERSION(
    "foo_resume",
    "1.0.1",
    "Remembers playback position per track and resumes from where you left off.\n\n"
    "To enable or disable: Preferences -> Advanced -> Tools -> foo_resume.\n\n"
    "Built by reda777"
);

VALIDATE_COMPONENT_FILENAME("foo_resume.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;

// ─── RAII FILE wrapper ──────────────────────────────────────────

struct FileCloser {
    void operator()(FILE* f) const { if (f) fclose(f); }
};
using unique_file = std::unique_ptr<FILE, FileCloser>;

// ─── GUIDs ─────────────────────────────────────────────────────

static const GUID guid_advconfig_branch =
{ 0x8a2b3c4d, 0x5e6f, 0x7a8b,
  {0x9c,0xad,0xbe,0xcf,0xd0,0xe1,0xf2,0x03} };

static const GUID guid_advconfig_enabled =
{ 0x1a2b3c4d, 0x5e6f, 0x7a8b,
  {0x9c,0xad,0xbe,0xcf,0xd0,0xe1,0xf2,0x04} };

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

// ─── Constants ──────────────────────────────────────────────────

static constexpr double   kMinResumeTime = 5.0;
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
static bool     g_loaded         = false;
static bool     g_dirty          = false;
static uint64_t g_last_save_tick = 0;
static std::string g_config_path;

static track_key g_current_key{};
static bool      g_has_current   = false;
static double    g_current_pos   = 0.0;
static bool      g_pending_seek  = false;
static double    g_expected_seek = 0.0;
static double    g_current_length = 0.0;

// ─── Config path ────────────────────────────────────────────────

static void init_config_path() {
    if (!g_config_path.empty()) return;
    pfc::string8 native;
    filesystem::g_get_native_path(core_api::get_profile_path(), native);
    native.add_filename("foo_resume_positions.txt");
    g_config_path = native.c_str();
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

    FILE* raw = nullptr;
    fopen_s(&raw, g_config_path.c_str(), "r");
    unique_file f(raw);
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f.get())) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();

        if (s.empty() || s[0] == '#') continue;

        size_t p1 = s.find('\t');
        if (p1 == std::string::npos) continue;
        size_t p2 = s.find('\t', p1 + 1);
        if (p2 == std::string::npos) continue;

        try {
            track_key key;
            key.path    = s.substr(0, p1);
            key.subsong = static_cast<t_uint32>(
                            std::stoul(s.substr(p1 + 1, p2 - p1 - 1)));
            double pos  = std::stod(s.substr(p2 + 1));

            if (pos > kMinResumeTime)
                g_positions[key] = {pos, ++g_access_counter};
        }
        catch (...) {}
    }
}

// ─── Save ───────────────────────────────────────────────────────

static void save_positions() {
    std::string temp = g_config_path + ".tmp";

    FILE* raw = nullptr;
    fopen_s(&raw, temp.c_str(), "w");
    unique_file f(raw);
    if (!f) return;

    fprintf(f.get(), "# foo_resume v1\n");

    size_t count = 0;
    for (const auto& kv : g_positions) {
        if (kv.second.position > kMinResumeTime) {
            fprintf(f.get(), "%s\t%u\t%.3f\n",
                kv.first.path.c_str(),
                kv.first.subsong,
                kv.second.position);
            if (++count >= kMaxEntries) break;
        }
    }

    f.reset(); // close before rename

    if (!MoveFileExA(temp.c_str(), g_config_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        FB2K_console_formatter() << "foo_resume: save failed, error "
                                 << GetLastError();
        DeleteFileA(temp.c_str());
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
    double m_seek;
    seek_callback(double v) : m_seek(v) {}

    void callback_run() override {
        static_api_ptr_t<playback_control> pc;
        if (pc->is_playing())
            pc->playback_seek(m_seek);
    }
};

// ─── Playback callback ──────────────────────────────────────────

namespace {

class resume_play_callback : public play_callback {
public:
    void on_playback_new_track(metadb_handle_ptr p_track) override {
        if (!g_advconfig_enabled.get()) return;

        init_config_path();
        load_positions();

        if (g_has_current && !g_pending_seek) {
            bool completed = g_current_length > 0.0 &&
                            g_current_pos >= (g_current_length - 2.0);
            if (completed) {
                g_positions.erase(g_current_key);
                g_dirty = true;
            } else if (g_current_pos > kMinResumeTime) {
                g_positions[g_current_key] = {g_current_pos, ++g_access_counter};
                prune_if_needed();
                g_dirty = true;
            }
        }

        const playable_location& loc = p_track->get_location();
        g_current_key.path    = loc.get_path();
        g_current_key.subsong = loc.get_subsong_index();
        g_current_pos         = 0.0;
        g_has_current         = true;
        g_pending_seek        = false;
        g_current_length      = p_track->get_length();

        auto it = g_positions.find(g_current_key);
        if (it != g_positions.end()) {
            double seek_to = it->second.position;
            it->second.last_used = ++g_access_counter;
            g_pending_seek  = true;
            g_expected_seek = seek_to;
            static_api_ptr_t<main_thread_callback_manager>()
                ->add_callback(new service_impl_t<seek_callback>(seek_to));
        }

        save_if_dirty();
    }

    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (!g_advconfig_enabled.get()) return;
        if (!g_has_current) return;

        if (reason == play_control::stop_reason_eof) {
            g_positions.erase(g_current_key);
            g_dirty = true;
        } else if (g_current_pos > kMinResumeTime) {
            g_positions[g_current_key] = {g_current_pos, ++g_access_counter};
            g_dirty = true;
        }

        save_if_dirty();
        g_has_current  = false;
        g_current_pos  = 0.0;
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
            play_callback::flag_on_playback_stop      |
            play_callback::flag_on_playback_time      |
            play_callback::flag_on_playback_seek,
            false);
    }

    void on_quit() override {
        if (g_advconfig_enabled.get() &&
            g_has_current &&
            g_current_pos > kMinResumeTime)
        {
            g_positions[g_current_key] = {g_current_pos, ++g_access_counter};
            g_dirty = true;
        }

        save_if_dirty(true);

        static_api_ptr_t<play_callback_manager>()
            ->unregister_callback(&g_callback);
    }
};

FB2K_SERVICE_FACTORY(resume_initquit);

}
