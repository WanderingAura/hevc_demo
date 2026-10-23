#include "client/ui.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "implot.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace hs {

constexpr size_t kFeedCap = 4096;

UiFeed::UiFeed() { rows_.reserve(kFeedCap); }

void UiFeed::client_row(const ClientRow& r) {
    std::lock_guard<std::mutex> lk(mu_);
    if (rows_.size() < kFeedCap) rows_.push_back(r);
}

void UiFeed::client_status(const ClientStatus& s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = s;
}

void UiFeed::take(std::vector<ClientRow>& rows, ClientStatus& status) {
    std::lock_guard<std::mutex> lk(mu_);
    rows.clear();
    rows.swap(rows_);
    status = status_;
}

namespace {

constexpr int kHist = 1800;  // ~60 s of frames at 30 fps
constexpr float kPanelW = 440;
constexpr float kPlotH = 330;
constexpr double kPlotSpanS = 20;

// Per-frame history for the charts: a struct-of-arrays ring that ImPlot reads with an offset.
struct History {
    float t[kHist], age_recv[kHist], age_show[kHist], queueing[kHist], rtt[kHist], size_kb[kHist], idr_kb[kHist],
        fsm[kHist];
    int head = 0, count = 0;
};

void push(History& h, const ClientRow& r, int64_t t0) {
    int i;
    if (h.count < kHist) {
        i = h.count++;
    } else {
        i = h.head;
        h.head = (h.head + 1) % kHist;
    }
    h.t[i] = float(double(r.capture_ts - t0) / kSec);
    h.age_recv[i] = float(r.age_recv) / kMs;
    h.age_show[i] = r.fate == uint8_t(Fate::Shown) ? float(r.age_show) / kMs : NAN;
    h.queueing[i] = float(r.queueing) / kMs;
    h.rtt[i] = float(r.rtt_min) / kMs;
    h.size_kb[i] = float(r.size) / 1024.f;
    h.idr_kb[i] = r.type ? h.size_kb[i] : NAN;
    h.fsm[i] = float(r.fsm_state);
}

float newest_t(const History& h) {
    if (!h.count) return 0;
    return h.t[(h.head + h.count - 1) % kHist];
}

// One live-tunable integer: edited in the UI, sent as a UiSet event on Enter / step.
struct LiveInt {
    const char* key;
    const char* label;
    int value;
    int step;
};

void post_set(const PostEvent& post, Clock& clock, const char* key, const std::string& value) {
    post(cli_ui_set(clock.now(), key, value.c_str()));
}

void live_ints(LiveInt* items, int n, const PostEvent& post, Clock& clock) {
    for (int i = 0; i < n; ++i) {
        LiveInt& it = items[i];
        if (ImGui::InputInt(it.label, &it.value, it.step, it.step * 10, ImGuiInputTextFlags_EnterReturnsTrue))
            post_set(post, clock, it.key, std::to_string(it.value));
    }
}

ImVec4 fsm_color(FsmState s) {
    switch (s) {
        case FsmState::Normal: return ImVec4(0.3f, 0.9f, 0.3f, 1);
        case FsmState::Catchup: return ImVec4(0.95f, 0.8f, 0.2f, 1);
        case FsmState::WaitIdr: return ImVec4(1.0f, 0.35f, 0.3f, 1);
    }
    return ImVec4(1, 1, 1, 1);
}

struct Controls {
    LiveInt server[8];
    LiveInt client[6];
    int preset;
    bool require_sync;
};

const char* const kPresets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium"};

Controls make_controls(const Config& c) {
    Controls k{
        {{"encoder.bitrate_kbps", "bitrate kbps", c.encoder.bitrate_kbps, 500},
         {"encoder.gop", "GOP", c.encoder.gop, 10},
         {"source.fps", "fps", c.source.fps, 1},
         {"source.sim_isp_delay_ms", "ISP delay ms", c.source.sim_isp_delay_ms, 5},
         {"source.sim_isp_jitter_ms", "ISP jitter ms", c.source.sim_isp_jitter_ms, 1},
         {"server.queue.max_frames", "queue frames", c.server.queue.max_frames, 1},
         {"server.queue.max_bytes", "queue bytes", c.server.queue.max_bytes, 100000},
         {"server.queue.max_age_ms", "queue max age ms", c.server.queue.max_age_ms, 10}},
        {{"client.t_stale_ms", "T_stale ms", c.client.t_stale_ms, 10},
         {"client.t_hard_ms", "T_hard ms", c.client.t_hard_ms, 10},
         {"client.q_max_ms", "Q_max ms", c.client.q_max_ms, 10},
         {"client.decode_queue", "decode queue", c.client.decode_queue, 1},
         {"client.catchup_max_frames", "catch-up frames", c.client.catchup_max_frames, 1},
         {"clock.sync_margin_ms", "sync margin ms (-1 auto)", c.clock.sync_margin_ms, 1}},
        0,
        c.clock.require_sync,
    };
    for (int i = 0; i < int(sizeof kPresets / sizeof kPresets[0]); ++i)
        if (c.encoder.preset == kPresets[i]) k.preset = i;
    return k;
}

void draw_status(const ClientStatus& s) {
    ImGui::Text("data: %s", s.data_connected ? "connected" : "disconnected");
    ImGui::SameLine();
    ImGui::TextColored(fsm_color(s.fsm), "  %s", fsm_name(s.fsm));
    ImGui::Text("frames rx %u   last seq %u   decode queue %u", s.frames_rx, s.last_seq, s.decode_queue_len);
    ImGui::Text("RTT %.2f ms (min %.2f)", double(s.rtt_last_ns) / kMs, double(s.rtt_min_ns) / kMs);
    ImGui::Text("clock: client %s (est %.2f ms)  server %s", s.client_sync.synced ? "synced" : "UNSYNCED",
                double(s.client_sync.est_error_ns) / kMs,
                !s.have_stats ? "?" : s.server.sync_ok ? "synced" : "UNSYNCED");
    if (s.ages_trusted)
        ImGui::Text("ages trusted, margin %.2f ms, anomalies %u", double(s.margin_ns) / kMs, s.anomalies);
    else
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "ages UNTRUSTED (queueing only), anomalies %u", s.anomalies);

    ImGui::SeparatorText("frames");
    for (uint32_t f = uint32_t(Fate::Shown); f < uint32_t(Fate::Count); ++f) {
        ImGui::Text("%-12s %u", fate_name(Fate(f)), s.fates[f]);
        if (f % 2 == 1 && f + 1 < uint32_t(Fate::Count)) ImGui::SameLine(200);
    }
    ImGui::SeparatorText("keyframe requests sent");
    for (uint32_t r = 0; r < uint32_t(KfReason::Count); ++r) {
        ImGui::Text("%-12s %u", kf_reason_name(KfReason(r)), s.kf_requests[r]);
        if (r % 2 == 0 && r + 1 < uint32_t(KfReason::Count)) ImGui::SameLine(200);
    }
    ImGui::SeparatorText("server");
    if (!s.have_stats) {
        ImGui::TextDisabled("no STATS yet");
    } else {
        const StatsMsg& st = s.server;
        ImGui::Text("queue %u frames / %u bytes   next seq %u", st.queue_frames, st.queue_bytes, st.next_seq);
        ImGui::Text("encode %.1f ms (avg %.1f)   IDRs %u   kf reqs %u", double(st.enc_last_ns) / kMs,
                    double(st.enc_avg_ns) / kMs, st.idr_count, st.keyframe_reqs);
        for (uint32_t d = 1; d < uint32_t(DropReason::Count); ++d) {
            ImGui::Text("drop %-12s %u", drop_reason_name(DropReason(d)), st.drops[d]);
            if (d % 2 == 1 && d + 1 < uint32_t(DropReason::Count)) ImGui::SameLine(200);
        }
    }
    if (s.have_ack)
        ImGui::Text("last ACK: %s=%s  %s  @seq %u", s.last_ack.key, s.last_ack.value,
                    err_name(Err(s.last_ack.status)), s.last_ack.applied_at_seq);
    if (s.local_key[0]) ImGui::Text("last local: %s  %s", s.local_key, err_name(Err(s.local_status)));
}

void draw_controls(const ClientStatus& s, Controls& k, const PostEvent& post, Clock& clock, ImVec2 disp) {
    ImGui::SetNextWindowPos(ImVec2(disp.x - kPanelW, 0));
    ImGui::SetNextWindowSize(ImVec2(kPanelW, disp.y));
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) draw_status(s);
    if (ImGui::CollapsingHeader("Server (live, CONFIG_SET)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(160);
        live_ints(k.server, 8, post, clock);
        if (ImGui::Combo("preset", &k.preset, kPresets, int(sizeof kPresets / sizeof kPresets[0])))
            post_set(post, clock, "encoder.preset", kPresets[k.preset]);
        ImGui::PopItemWidth();
        if (ImGui::Button("Force IDR")) post(cli_event(ClientEvent::UiForceIdr, clock.now()));
    }
    if (ImGui::CollapsingHeader("Client (live)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(160);
        live_ints(k.client, 6, post, clock);
        ImGui::PopItemWidth();
        if (ImGui::Checkbox("require sync", &k.require_sync))
            post_set(post, clock, "clock.require_sync", k.require_sync ? "true" : "false");
    }
    ImGui::End();
}

void draw_plots(const History& h, const Config& cfg, ImVec2 disp) {
    ImGui::SetNextWindowPos(ImVec2(0, disp.y - kPlotH));
    ImGui::SetNextWindowSize(ImVec2(disp.x - kPanelW, kPlotH));
    ImGui::Begin("Plots", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar);
    const double t1 = newest_t(h), t0 = t1 - kPlotSpanS;
    ImPlotSpec ring;
    ring.Offset = h.head;
    ImPlotSpec ring_nan = ring;
    ring_nan.Flags = ImPlotLineFlags_SkipNaN;
    if (ImPlot::BeginSubplots("##plots", 2, 2, ImVec2(-1, -1))) {
        if (ImPlot::BeginPlot("frame age (ms)")) {
            ImPlot::SetupAxes("capture time (s)", nullptr, 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
            ImPlot::PlotLine("at receive", h.t, h.age_recv, h.count, ring);
            ImPlot::PlotLine("on screen", h.t, h.age_show, h.count, ring_nan);
            double lines[2] = {double(cfg.client.t_stale_ms), double(cfg.client.t_hard_ms)};
            ImPlotSpec horiz;
            horiz.Flags = ImPlotInfLinesFlags_Horizontal;
            ImPlot::PlotInfLines("T_stale / T_hard", lines, 2, horiz);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("network (ms)")) {
            ImPlot::SetupAxes("capture time (s)", nullptr, 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
            ImPlot::PlotLine("queueing", h.t, h.queueing, h.count, ring);
            ImPlot::PlotLine("RTT min", h.t, h.rtt, h.count, ring);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("frame size (KB)")) {
            ImPlot::SetupAxes("capture time (s)", nullptr, 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
            ImPlot::PlotLine("size", h.t, h.size_kb, h.count, ring);
            ImPlotSpec idr = ring;
            idr.Marker = ImPlotMarker_Circle;
            ImPlot::PlotScatter("IDR", h.t, h.idr_kb, h.count, idr);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("FSM (0 WAIT_IDR, 1 NORMAL, 2 CATCHUP)")) {
            ImPlot::SetupAxes("capture time (s)", nullptr);
            ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -0.2, 2.2, ImPlotCond_Always);
            ImPlot::PlotStairs("state", h.t, h.fsm, h.count, ring);
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
    ImGui::End();
}

SDL_FRect fit(float tex_w, float tex_h, float area_w, float area_h) {
    float s = std::fmin(area_w / tex_w, area_h / tex_h);
    if (s <= 0) s = 0;
    float w = tex_w * s, h = tex_h * s;
    return SDL_FRect{(area_w - w) / 2, (area_h - h) / 2, w, h};
}

}  // namespace

void run_ui(const Config& cfg, FrameMailbox& mailbox, UiFeed& feed, Clock& clock, const PostEvent& post,
            const std::atomic<bool>& quit) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "ui: SDL_Init: %s\n", SDL_GetError());
        return;
    }
    SDL_Window* win = SDL_CreateWindow("h265sim client", 1600, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) {
        std::fprintf(stderr, "ui: cannot create window/renderer: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }
    // VSync on can add up to a refresh of latency; off can tear. Measure both.
    SDL_SetRenderVSync(ren, cfg.client.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    AVFrame* frame = av_frame_alloc();
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<ClientRow> rows;
    rows.reserve(kFeedCap);
    ClientStatus status{};
    std::unique_ptr<History> hist = std::make_unique<History>();
    int64_t t0 = 0;
    Controls controls = make_controls(cfg);
    Config shown_cfg = cfg;  // thresholds drawn on the age plot
    uint64_t last_render = 0;

    for (bool done = false; !done && !quit;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
        }
        uint32_t seq = 0;
        const bool fresh = mailbox.take(frame, seq);
        const uint64_t now_ms = SDL_GetTicks();
        if (!fresh && now_ms - last_render < 8) {  // redraw controls at ~120 Hz, video immediately
            SDL_Delay(1);
            continue;
        }
        last_render = now_ms;

        if (fresh && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P)) {
            if (!tex || tex_w != frame->width || tex_h != frame->height) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame->width,
                                        frame->height);
                tex_w = frame->width;
                tex_h = frame->height;
            }
            SDL_UpdateYUVTexture(tex, nullptr, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                 frame->data[2], frame->linesize[2]);
        }
        feed.take(rows, status);
        for (const ClientRow& r : rows) {
            if (!t0) t0 = r.capture_ts;
            push(*hist, r, t0);
        }
        shown_cfg.client.t_stale_ms = controls.client[0].value;
        shown_cfg.client.t_hard_ms = controls.client[1].value;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImVec2 disp = io.DisplaySize;
        draw_controls(status, controls, post, clock, disp);
        draw_plots(*hist, shown_cfg, disp);
        ImGui::Render();

        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(ren, 12, 12, 14, 255);
        SDL_RenderClear(ren);
        if (tex) {
            SDL_FRect dst = fit(float(tex_w), float(tex_h), disp.x - kPanelW, disp.y - kPlotH);
            SDL_RenderTexture(ren, tex, nullptr, &dst);
        }
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
        if (fresh) {  // display_ts: right after the frame was presented
            ClientEvent ev = cli_event(ClientEvent::Displayed, clock.now());
            ev.displayed.seq = seq;
            post(ev);
        }
    }

    av_frame_free(&frame);
    if (tex) SDL_DestroyTexture(tex);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

}  // namespace hs
