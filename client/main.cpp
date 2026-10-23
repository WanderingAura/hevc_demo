// h265sim-client: low-latency receiver, wired to real ports.
//   h265sim-client [config.ini] [section.key=value ...] [--headless] [--seconds=N]
// Threads: SDL/UI on main, every decision on the Asio io thread, libavcodec on its worker.
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "client/decoder_av.hpp"
#include "client/run.hpp"
#include "client/ui.hpp"
#include "common/real.hpp"

extern "C" {
#include <libavutil/log.h>
}

using namespace hs;
using asio::ip::tcp;

namespace {

using Deliver = std::function<void(const ClientEvent&)>;
constexpr auto kRetry = std::chrono::seconds(1);

// Data channel: connects, reconnects every second, delivers bytes as they arrive.
class TcpDataClient final : public ByteRx {
public:
    TcpDataClient(asio::io_context& io, const TransportCfg& t, Clock& clock, Deliver deliver)
        : io_(io), cfg_(t), clock_(clock), deliver_(std::move(deliver)), retry_(io) {
        connect();
    }

    void close() override {
        if (sock_) drop();
    }

private:
    void connect() {
        sock_ = std::make_unique<tcp::socket>(io_);
        asio::error_code ec;
        tcp::resolver resolver(io_);
        auto eps = resolver.resolve(cfg_.host, std::to_string(cfg_.data_port), ec);
        if (ec) return retry();
        asio::async_connect(*sock_, eps, [this](const asio::error_code& ec2, const tcp::endpoint&) {
            if (ec2) {
                sock_.reset();
                return retry();
            }
            asio::error_code ignore;
            sock_->set_option(tcp::no_delay(true), ignore);
            ++gen_;
            std::fprintf(stderr, "data: connected\n");
            deliver_(cli_event(ClientEvent::DataConnected, clock_.now()));
            read();
        });
    }

    void retry() {
        retry_.expires_after(kRetry);
        retry_.async_wait([this](const asio::error_code& ec) {
            if (!ec) connect();
        });
    }

    void read() {
        uint32_t g = gen_;
        sock_->async_read_some(asio::buffer(buf_, sizeof buf_), [this, g](const asio::error_code& ec, size_t n) {
            if (g != gen_) return;
            if (ec) return drop();
            // recv timestamp: when the bytes reached userspace
            deliver_(cli_bytes(clock_.now(), Bytes{buf_, uint32_t(n)}));
            if (g == gen_) read();
        });
    }

    void drop() {
        asio::error_code ec;
        sock_->close(ec);
        sock_.reset();
        ++gen_;
        std::fprintf(stderr, "data: disconnected\n");
        deliver_(cli_event(ClientEvent::DataDisconnected, clock_.now()));
        retry();
    }

    asio::io_context& io_;
    TransportCfg cfg_;
    Clock& clock_;
    Deliver deliver_;
    asio::steady_timer retry_;
    std::unique_ptr<tcp::socket> sock_;
    uint32_t gen_ = 0;
    uint8_t buf_[1 << 16];
};

struct Tee final : LogSink {
    LogSink* a;
    LogSink* b;
    Tee(LogSink* x, LogSink* y) : a(x), b(y) {}
    void client_row(const ClientRow& r) override {
        a->client_row(r);
        b->client_row(r);
    }
    void client_status(const ClientStatus& s) override {
        a->client_status(s);
        b->client_status(s);
    }
};

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!load_config(cfg, argc, argv)) return 1;
    av_log_set_level(AV_LOG_ERROR);
    const bool headless = !flag(argc, argv, "--headless").empty();
    const std::string seconds = flag(argc, argv, "--seconds");

    asio::io_context io;
    auto work = asio::make_work_guard(io);
    RealClock clock;
    static ClientState state;  // large: keep it off the stack
    client_init(state, cfg);

    CsvLog csv(cfg.client.csv, kClientCsvHeader);
    UiFeed ui_feed;
    Tee tee(&csv, &ui_feed);
    LogSink* log = headless ? static_cast<LogSink*>(&csv) : &tee;

    ClientPorts ports{};
    Deliver deliver = [&](const ClientEvent& ev) { client_dispatch(state, ev, ports); };
    auto post = [&](const ClientEvent& ev) { asio::post(io, [&deliver, ev] { deliver(ev); }); };

    AsioTimers timers(io, kCliTimerCount, [&](uint8_t id) { deliver(cli_timer(clock.now(), id)); });
    FrameMailbox mailbox;
    AvDecoder decoder(cfg.client.decode_threads, clock, post, headless ? nullptr : &mailbox);
    TcpDataClient data(io, cfg.transport, clock, deliver);

    // Control channel: its own TCP connection, so PONG/STATS never queue behind video.
    asio::steady_timer ctrl_retry(io);
    std::function<void()> connect_ctrl;
    CtrlConn ctrl([&](const CtrlMsg& m) { deliver(cli_ctrl(clock.now(), m)); },
                  [&] {
                      ctrl_retry.expires_after(kRetry);
                      ctrl_retry.async_wait([&](const asio::error_code& ec) {
                          if (!ec) connect_ctrl();
                      });
                  });
    connect_ctrl = [&] {
        auto sock = std::make_shared<tcp::socket>(io);
        asio::error_code ec;
        auto eps = tcp::resolver(io).resolve(cfg.transport.host, std::to_string(cfg.transport.control_port), ec);
        if (ec) {
            ctrl_retry.expires_after(kRetry);
            ctrl_retry.async_wait([&](const asio::error_code& e) {
                if (!e) connect_ctrl();
            });
            return;
        }
        asio::async_connect(*sock, eps, [&, sock](const asio::error_code& e, const tcp::endpoint&) {
            if (!e) {
                ctrl.attach(std::move(*sock));
                return;
            }
            ctrl_retry.expires_after(kRetry);
            ctrl_retry.async_wait([&](const asio::error_code& e2) {
                if (!e2) connect_ctrl();
            });
        });
    };
    connect_ctrl();

    ports = ClientPorts{&clock, &timers, &data, &ctrl, &decoder, log};
    std::fprintf(stderr, "h265sim-client: %s:%d (control %d), T_stale %d ms, T_hard %d ms%s\n",
                 cfg.transport.host.c_str(), cfg.transport.data_port, cfg.transport.control_port,
                 cfg.client.t_stale_ms, cfg.client.t_hard_ms, headless ? ", headless" : "");

    std::atomic<bool> quit{false};
    auto stop = [&] {
        quit = true;
        io.stop();
    };
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code&, int) { stop(); });
    asio::steady_timer stop_timer(io);
    if (!seconds.empty()) {
        stop_timer.expires_after(std::chrono::seconds(std::atoi(seconds.c_str())));
        stop_timer.async_wait([&](const asio::error_code& ec) {
            if (!ec) stop();
        });
    }

    post(cli_event(ClientEvent::Start, clock.now()));
    if (headless) {
        io.run();
    } else {
        std::thread logic([&] { io.run(); });
        run_ui(cfg, mailbox, ui_feed, clock, post, quit);
        io.stop();
        logic.join();
    }
    return 0;
}
