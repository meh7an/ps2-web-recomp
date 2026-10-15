#include "window.hpp"

#include "input.hpp"
#include "ps2/system.hpp"

#include <SDL.h>
#include <SDL_syswm.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace input = ps2::input;

// The input module names keys by USB HID usage, as SDL's scancodes do; check each key it binds.
constexpr bool same_key(input::Key key, SDL_Scancode code) { return static_cast<int>(key) == static_cast<int>(code); }
static_assert(same_key(input::kKeyA, SDL_SCANCODE_A) && same_key(input::kKeyD, SDL_SCANCODE_D) && same_key(input::kKeyE, SDL_SCANCODE_E) &&
              same_key(input::kKeyF, SDL_SCANCODE_F) && same_key(input::kKeyG, SDL_SCANCODE_G) && same_key(input::kKeyH, SDL_SCANCODE_H) &&
              same_key(input::kKeyI, SDL_SCANCODE_I) && same_key(input::kKeyJ, SDL_SCANCODE_J) && same_key(input::kKeyK, SDL_SCANCODE_K) &&
              same_key(input::kKeyL, SDL_SCANCODE_L) && same_key(input::kKeyQ, SDL_SCANCODE_Q) && same_key(input::kKeyS, SDL_SCANCODE_S) &&
              same_key(input::kKeyT, SDL_SCANCODE_T) && same_key(input::kKeyW, SDL_SCANCODE_W) && same_key(input::kKey1, SDL_SCANCODE_1) &&
              same_key(input::kKey2, SDL_SCANCODE_2) && same_key(input::kKey3, SDL_SCANCODE_3) && same_key(input::kKey4, SDL_SCANCODE_4) &&
              same_key(input::kKeyReturn, SDL_SCANCODE_RETURN) && same_key(input::kKeyBackspace, SDL_SCANCODE_BACKSPACE) &&
              same_key(input::kKeyRight, SDL_SCANCODE_RIGHT) && same_key(input::kKeyLeft, SDL_SCANCODE_LEFT) &&
              same_key(input::kKeyDown, SDL_SCANCODE_DOWN) && same_key(input::kKeyUp, SDL_SCANCODE_UP));

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, input::kCross},          {SDL_CONTROLLER_BUTTON_B, input::kCircle},
    {SDL_CONTROLLER_BUTTON_X, input::kSquare},         {SDL_CONTROLLER_BUTTON_Y, input::kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, input::kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, input::kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, input::kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, input::kR3},
    {SDL_CONTROLLER_BUTTON_BACK, input::kSelect},      {SDL_CONTROLLER_BUTTON_START, input::kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, input::kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, input::kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, input::kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, input::kRight},
};

// No mouse button is bound by default: GoW2 has no camera to aim (spec 011).
constexpr std::span<const input::MouseBinding> kMouseButtons{};
constexpr input::Settings kInputSettings{};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
        target.kind = ps2::WindowTarget::Win32;
        target.display = info.info.win.hinstance;
        target.window = reinterpret_cast<std::uintptr_t>(info.info.win.window);
        break;
#endif
    default: return false;
    }
    return ps2::attach_window(target);
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

// a new controller takes the first free port, the second one plugs in player 2
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_pads)
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
    for (int port = 0; port < 2; ++port)
        if (!g_pads[port]) {
            g_pads[port] = SDL_GameControllerOpen(device);
            return;
        }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (port > 0 && plugged[port] != (pad != nullptr)) {
        plugged[port] = pad != nullptr;
        ps2::set_pad_connected(port, plugged[port]);
    }
    input::Held held;
    if (port == 0) {
        int count = 0;
        const Uint8 *keys = SDL_GetKeyboardState(&count);
        held.digital = input::from_keys(keys, static_cast<std::size_t>(count), input::kDefaultKeys);
        held.digital |= input::from_mouse(SDL_GetMouseState(nullptr, nullptr), kMouseButtons);
    }
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (SDL_GameControllerGetButton(pad, b.button)) held.digital.buttons |= b.mask;
        held.controller = true;
        held.left_x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        held.left_y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        held.right_x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        held.right_y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        held.left_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        held.right_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    }
    rumble(port);
    static input::PadState last[2];
    static bool sent[2] = {};
    const input::PadState state = input::resolve(held, kInputSettings);
    if (sent[port] && state == last[port]) return;
    sent[port] = true;
    last[port] = state;
    ps2::set_pad_pressure(8, state.l2, port);
    ps2::set_pad_pressure(9, state.r2, port);
    ps2::set_pad_buttons(state.buttons, port);
    ps2::set_pad_axis(0, state.rx, port);
    ps2::set_pad_axis(1, state.ry, port);
    ps2::set_pad_axis(2, state.lx, port);
    ps2::set_pad_axis(3, state.ly, port);
}

// Only records the size: the GPU code rebuilds the swapchain to the newest one at most every
// 100 ms, so this can run on every size event of a resize.
void apply_window_size() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
}

void poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) ps2::halt("window closed");
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
            const bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) apply_window_size();
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            for (SDL_GameController *&pad : g_pads)
                if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
    }
    poll_port(0);
    poll_port(1);
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(w) * 3 / 4);
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, rgba, static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
}

void present() {
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) show_pixels(g_pixels.data(), w, h);
}

// The SPU2 writes this ring on the emulation thread and SDL's callback reads it, as in host/web.cpp.
// The callback counts every gap it fills with silence, so underruns are measured, not guessed.
constexpr std::uint32_t kAudioRate = 48000;
constexpr std::uint32_t kAudioFrames = 1u << 14;
constexpr std::uint32_t kAudioTarget = 80 * kAudioRate / 1000;

struct AudioRing {
    std::atomic<std::uint32_t> write{0}, read{0};
    std::atomic<std::uint64_t> underruns{0}, silent_frames{0}, dropped_frames{0};
    std::int16_t samples[kAudioFrames * 2];
};

AudioRing g_ring;
bool g_audio_playing = false;

std::uint32_t audio_queued() { return g_ring.write.load(std::memory_order_acquire) - g_ring.read.load(std::memory_order_acquire); }

double frames_to_ms(std::uint64_t frames) { return static_cast<double>(frames) * 1000.0 / kAudioRate; }

void SDLCALL play_audio(void *, Uint8 *stream, int len) {
    auto *out = reinterpret_cast<std::int16_t *>(stream);
    const std::uint32_t wanted = static_cast<std::uint32_t>(len) / 4;
    std::uint32_t read = g_ring.read.load(std::memory_order_relaxed);
    const std::uint32_t taken = std::min(wanted, g_ring.write.load(std::memory_order_acquire) - read);
    for (std::uint32_t i = 0; i < taken; ++i, ++read) {
        const std::uint32_t at = (read & (kAudioFrames - 1)) * 2;
        out[i * 2] = g_ring.samples[at];
        out[i * 2 + 1] = g_ring.samples[at + 1];
    }
    g_ring.read.store(read, std::memory_order_release);
    if (taken == wanted) return;
    std::memset(out + taken * 2, 0, (wanted - taken) * 4u);
    g_ring.underruns.fetch_add(1, std::memory_order_relaxed);
    g_ring.silent_frames.fetch_add(wanted - taken, std::memory_order_relaxed);
}

void pace() {
    if (g_audio) {
        // Playback starts once the target is queued, so the wait before the first samples is no underrun.
        if (!g_audio_playing && audio_queued() >= kAudioTarget) {
            g_audio_playing = true;
            SDL_PauseAudioDevice(g_audio, 0);
        }
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is queued.
        while (audio_queued() > kAudioTarget) SDL_Delay(1);
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / (ps2::g_options.pal ? 50 : 60);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (g_last && now - g_last < period) {
        SDL_Delay(static_cast<Uint32>((period - (now - g_last)) * 1000 / frequency));
        g_last += period;
    } else {
        g_last = now;
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    // FILETIME counts 100 ns ticks.
    const auto ticks = [](const FILETIME &t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double resident_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) return 0.0;
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

using Clock = std::chrono::steady_clock;

double ms_between(Clock::time_point from, Clock::time_point to) { return std::chrono::duration<double, std::milli>(to - from).count(); }

// Intervals between marks made on one thread, collected by the statistics on another.
struct Intervals {
    std::mutex lock;
    std::vector<float> fresh;
    Clock::time_point last{};
    std::uint64_t count = 0;

    void mark() {
        const Clock::time_point now = Clock::now();
        const std::lock_guard<std::mutex> hold(lock);
        if (count++) fresh.push_back(static_cast<float>(ms_between(last, now)));
        last = now;
    }
    std::vector<float> take() {
        const std::lock_guard<std::mutex> hold(lock);
        return std::exchange(fresh, {});
    }
    std::uint64_t marks() {
        const std::lock_guard<std::mutex> hold(lock);
        return count;
    }
};

Intervals g_vblanks, g_presents;
std::atomic<double> g_emulation_idle_ms{0.0}, g_render_idle_ms{0.0};
// gpu_busy_ms reads counters the presenting thread owns, so that thread copies it here.
std::atomic<double> g_gpu_ms{0.0};
bool g_threads = false, g_vu_thread = false;

void add_ms(std::atomic<double> &total, Clock::time_point since) {
    total.store(total.load(std::memory_order_relaxed) + ms_between(since, Clock::now()), std::memory_order_relaxed);
}

// The cumulative counters at one moment; statistics are differences between two samples.
struct Sample {
    Clock::time_point at;
    std::uint64_t frame = 0, presents = 0, underruns = 0, silent = 0, dropped = 0, vu1_interp = 0;
    double emulation_idle = 0.0, render_idle = 0.0, vu_idle = 0.0, iop = 0.0, gpu = 0.0, cpu = 0.0;
};

Sample sample() {
    Sample s;
    s.at = Clock::now();
    s.frame = ps2::frame_count();
    s.presents = g_presents.marks();
    s.underruns = g_ring.underruns.load(std::memory_order_relaxed);
    s.silent = g_ring.silent_frames.load(std::memory_order_relaxed);
    s.dropped = g_ring.dropped_frames.load(std::memory_order_relaxed);
    s.emulation_idle = g_emulation_idle_ms.load(std::memory_order_relaxed);
    s.render_idle = g_render_idle_ms.load(std::memory_order_relaxed);
    s.vu_idle = ps2::vu_thread_idle_ms();
    s.iop = ps2::iop_busy_ms();
    s.gpu = g_gpu_ms.load(std::memory_order_relaxed);
    s.cpu = process_cpu_seconds();
    s.vu1_interp = ps2::vu1_interpreted_calls();
    return s;
}

struct Spread {
    double mean = 0.0, p99 = 0.0, max = 0.0;
};

Spread spread(std::vector<float> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const float v : values) sum += v;
    const std::size_t p99 = (values.size() * 99 + 99) / 100 - 1;
    return {sum / static_cast<double>(values.size()), values[p99], values.back()};
}

// Rates per second, intervals in ms, and each thread's busy ms per emulated frame. Without the
// threaded runner the GS works on the emulation thread, and without a VU thread so does VU1.
struct Stats {
    std::uint64_t frame = 0, underruns = 0;
    double fps = 0.0, shown = 0.0, ee = 0.0, iop = 0.0, vu1 = 0.0, gs = 0.0, gpu = 0.0, cpu = 0.0, silence_ms = 0.0, dropped_ms = 0.0;
    // VU1 programs with no compiled match run on the interpreter; these count its entries.
    double vu1_interp = 0.0, vu1_interp_per_second = 0.0;
    Spread frame_ms, present_ms;
};

Stats measure(const Sample &from, const Sample &to, std::vector<float> vblanks, std::vector<float> presents) {
    Stats s;
    const double wall = std::max(ms_between(from.at, to.at), 1e-3);
    const double frames = static_cast<double>(std::max<std::uint64_t>(to.frame - from.frame, 1));
    const double iop = to.iop - from.iop;
    s.frame = to.frame;
    s.fps = static_cast<double>(to.frame - from.frame) * 1000.0 / wall;
    s.shown = static_cast<double>(to.presents - from.presents) * 1000.0 / wall;
    s.frame_ms = spread(std::move(vblanks));
    s.present_ms = spread(std::move(presents));
    s.ee = (wall - (to.emulation_idle - from.emulation_idle) - iop) / frames;
    s.iop = iop / frames;
    s.vu1 = g_vu_thread ? (wall - (to.vu_idle - from.vu_idle)) / frames : 0.0;
    s.gs = g_threads ? (wall - (to.render_idle - from.render_idle)) / frames : 0.0;
    s.gpu = (to.gpu - from.gpu) / frames;
    s.cpu = (to.cpu - from.cpu) * 1e5 / wall;
    s.underruns = to.underruns - from.underruns;
    s.silence_ms = frames_to_ms(to.silent - from.silent);
    s.dropped_ms = frames_to_ms(to.dropped - from.dropped);
    s.vu1_interp = static_cast<double>(to.vu1_interp - from.vu1_interp) / frames;
    s.vu1_interp_per_second = static_cast<double>(to.vu1_interp - from.vu1_interp) * 1000.0 / wall;
    return s;
}

void print_stats(const char *label, const Stats &s) {
    char audio[16] = "none";
    if (g_audio) std::snprintf(audio, sizeof audio, "%.0f", frames_to_ms(audio_queued()));
    std::fprintf(stderr,
                 "[stats] %sframe=%llu fps=%.1f shown=%.1f frame_ms=%.2f/%.2f/%.2f present_ms=%.2f/%.2f/%.2f ee=%.2f iop=%.2f vu1=%.2f gs=%.2f gpu=%.2f "
                 "vu1_interp=%.2f audio_ms=%s underruns=%llu silence_ms=%.1f dropped_ms=%.1f\n",
                 label, static_cast<unsigned long long>(s.frame), s.fps, s.shown, s.frame_ms.mean, s.frame_ms.p99, s.frame_ms.max, s.present_ms.mean, s.present_ms.p99,
                 s.present_ms.max, s.ee, s.iop, s.vu1, s.gs, s.gpu, s.vu1_interp, audio, static_cast<unsigned long long>(s.underruns), s.silence_ms, s.dropped_ms);
}

// The emulation thread writes the title once a second; show_speed sets it on the window's thread.
std::mutex g_title_lock;
std::string g_title;

void publish_title(const Stats &s) {
    char title[256];
    std::snprintf(title, sizeof title,
                  "ps2-web-recomp  %.1f fps  EE %.1f  IOP %.1f  VU1 %.1f  GS %.1f  GPU %.1f ms  VU1 interpreted %.0f/s  CPU %.0f%%  RAM %.0f MB  audio %.0f ms  underruns %llu",
                  s.fps, s.ee, s.iop, s.vu1, s.gs, s.gpu, s.vu1_interp_per_second, s.cpu, resident_mb(), g_audio ? frames_to_ms(audio_queued()) : 0.0,
                  static_cast<unsigned long long>(g_ring.underruns.load(std::memory_order_relaxed)));
    const std::lock_guard<std::mutex> hold(g_title_lock);
    g_title = title;
}

bool g_stats_started = false;
Sample g_run_start, g_second_start;
std::vector<float> g_run_vblanks, g_run_presents;

// Called on the emulation thread after each vblank: one [stats] line per second of wall time.
void report_stats() {
    if (!g_stats_started) {
        g_run_start = g_second_start = sample();
        g_stats_started = true;
        return;
    }
    if (ms_between(g_second_start.at, Clock::now()) < 1000.0) return;
    const Sample now = sample();
    std::vector<float> vblanks = g_vblanks.take(), presents = g_presents.take();
    g_run_vblanks.insert(g_run_vblanks.end(), vblanks.begin(), vblanks.end());
    g_run_presents.insert(g_run_presents.end(), presents.begin(), presents.end());
    const Stats s = measure(g_second_start, now, std::move(vblanks), std::move(presents));
    print_stats("", s);
    publish_title(s);
    g_second_start = now;
}

// The whole run, once the emulation has stopped.
void report_total() {
    if (!g_stats_started) return;
    const Sample now = sample();
    std::vector<float> vblanks = g_vblanks.take(), presents = g_presents.take();
    g_run_vblanks.insert(g_run_vblanks.end(), vblanks.begin(), vblanks.end());
    g_run_presents.insert(g_run_presents.end(), presents.begin(), presents.end());
    print_stats("total ", measure(g_run_start, now, g_run_vblanks, g_run_presents));
}

void idle_pace() {
    const Clock::time_point start = Clock::now();
    pace();
    add_ms(g_emulation_idle_ms, start);
}

// Runs just before each present.
void show_speed() {
    g_presents.mark();
    g_gpu_ms.store(ps2::gpu_busy_ms(), std::memory_order_relaxed);
    std::string title;
    {
        const std::lock_guard<std::mutex> hold(g_title_lock);
        title.swap(g_title);
    }
    if (!title.empty()) SDL_SetWindowTitle(g_window, title.c_str());
}

void on_vblank() {
    g_vblanks.mark();
    show_speed();
    poll();
    present();
    idle_pace();
    report_stats();
}

// One pass of the threaded runner's GS render loop: a finished frame, or up to 8 ms of queued work.
ps2::Rendered render_pass() {
    static const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    static bool active = false;
    if (active) return ps2::Rendered::busy;
    active = true;
    const ps2::Rendered rendered = ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0);
    active = false;
    return rendered;
}

// Render passes run on exposed events, printed at exit. Inside Windows' move and size loops the
// main thread never waits for work, so the gs time per frame says nothing about these passes.
std::uint64_t g_exposed_passes = 0, g_exposed_presents = 0;
double g_exposed_ms = 0.0;

// Windows holds the main thread in its move and size loops. SDL's 10 ms timer keeps queuing exposed
// events there, so rendering on each keeps the GS queue moving and the emulation running. Sizes are
// passed on as they are queued, since poll() sees them only once the loop ends.
int SDLCALL render_in_modal_loops(void *, SDL_Event *e) {
    if (e->type != SDL_WINDOWEVENT) return 1;
    if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) apply_window_size();
    if (e->window.event == SDL_WINDOWEVENT_EXPOSED) {
        const Clock::time_point start = Clock::now();
        if (render_pass() == ps2::Rendered::presented) ++g_exposed_presents;
        ++g_exposed_passes;
        g_exposed_ms += ms_between(start, Clock::now());
    }
    return 1;
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    std::uint32_t write = g_ring.write.load(std::memory_order_relaxed);
    const std::uint32_t room = kAudioFrames - (write - g_ring.read.load(std::memory_order_acquire));
    const std::uint32_t taken = static_cast<std::uint32_t>(std::min<std::size_t>(frames, room));
    for (std::uint32_t i = 0; i < taken; ++i, ++write) {
        const std::uint32_t at = (write & (kAudioFrames - 1)) * 2;
        g_ring.samples[at] = samples[i * 2];
        g_ring.samples[at + 1] = samples[i * 2 + 1];
    }
    g_ring.write.store(write, std::memory_order_release);
    if (taken < frames) g_ring.dropped_frames.fetch_add(frames - taken, std::memory_order_relaxed);
}

// Pausing first keeps the drained ring after the last frame from counting as an underrun.
void finish_audio() {
    if (!g_audio) return;
    SDL_PauseAudioDevice(g_audio, 1);
    std::fprintf(stderr, "[audio] underruns=%llu silence_ms=%.1f dropped_ms=%.1f\n", static_cast<unsigned long long>(g_ring.underruns.load()),
                 frames_to_ms(g_ring.silent_frames.load()), frames_to_ms(g_ring.dropped_frames.load()));
}

}

bool open_window(bool fullscreen) {
    // Over Bluetooth, DualShock 4 and DualSense pads rumble only in SDL's enhanced report mode,
    // which these hints allow; over USB they always rumble.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
    g_window = SDL_CreateWindow("ps2-web-recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = play_audio;
    // The device opens paused; pace() starts it once enough sound is queued.
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        std::fprintf(stderr, "[audio] %d Hz, %d channels, %d frames per callback\n", have.freq, have.channels, have.samples);
        ps2::set_audio_sink(queue_audio);
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
    return true;
}

int run_window(const std::function<int()> &emulate) {
    ps2::init_graphics();
    g_gpu_present = attach_gpu();
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        const int result = emulate();
        finish_audio();
        report_total();
        return result;
    }
    g_threads = true;
    ps2::g_options.on_vblank = [] {
        g_vblanks.mark();
        ps2::post_present();
        idle_pace();
        report_stats();
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        g_vu_thread = true;
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
    std::atomic<bool> done{false};
    int result = 0;
    std::thread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    SDL_AddEventWatch(render_in_modal_loops, nullptr);
    while (!done.load()) {
        poll();
        if (render_pass() == ps2::Rendered::idle) {
            const Clock::time_point start = Clock::now();
            ps2::wait_render_work(4);
            add_ms(g_render_idle_ms, start);
        }
    }
    SDL_DelEventWatch(render_in_modal_loops, nullptr);
    std::fprintf(stderr, "[window] %llu render passes on exposed events in %.1f ms, %llu presents\n", static_cast<unsigned long long>(g_exposed_passes), g_exposed_ms,
                 static_cast<unsigned long long>(g_exposed_presents));
    emulation.join();
    finish_audio();
    report_total();
    SDL_Quit();
    std::fflush(nullptr);
    std::_Exit(result);
}
