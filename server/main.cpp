// h265sim-server: the camera simulator, wired to real ports.
//   h265sim-server [config.ini] [section.key=value ...] [--dump=out.h265] [--seconds=N]
// Threads: the Asio io thread runs every decision; libx265 runs on its worker.
#include <cstdio>
#include <cstdlib>

#include "common/real.hpp"
#include "server/encoder_av.hpp"
#include "server/run.hpp"
#include "server/source.hpp"

extern "C" {
#include <libavutil/log.h>
}

using namespace hs;
using asio::ip::tcp;

namespace {

using Deliver = std::function<void(const ServerEvent&)>;

// Data channel: one client at a time; a new connection replaces the old one.
class TcpDataServer final : public ByteTx {
public:
    TcpDataServer(asio::io_context& io, const TransportCfg& t, Clock& clock, Deliver deliver)
        : acceptor_(io, tcp::endpoint(tcp::v4(), uint16_t(t.data_port))), cfg_(t), clock_(clock),
          deliver_(std::move(deliver)) {
        accept();
    }

    void write(Bytes frame) override {
        uint32_t g = gen_;
        asio::async_write(*sock_, asio::buffer(frame.p, frame.n), [this, g](const asio::error_code& ec, size_t) {
            if (g != gen_) return;
            if (ec) return drop();
            deliver_(srv_event(ServerEvent::WriteDone, clock_.now()));
            // Choose the next frame only once the kernel has room (TCP_NOTSENT_LOWAT),
            // so the drop decision is made here with fresh information.
            sock_->async_wait(tcp::socket::wait_write, [this, g](const asio::error_code& ec2) {
                if (g != gen_) return;
                if (ec2) return drop();
                deliver_(srv_event(ServerEvent::Writable, clock_.now()));
            });
        });
    }

private:
    void accept() {
        acceptor_.async_accept([this](const asio::error_code& ec, tcp::socket s) {
            if (!ec) {
                if (sock_) drop();
                tune_socket(s, cfg_);
                sock_ = std::make_unique<tcp::socket>(std::move(s));
                ++gen_;
                std::fprintf(stderr, "data: client connected\n");
                deliver_(srv_event(ServerEvent::DataConnected, clock_.now()));
                watch();
            }
            accept();
        });
    }

    // The client never sends on this socket; a read completing means it closed.
    void watch() {
        uint32_t g = gen_;
        sock_->async_read_some(asio::buffer(scratch_), [this, g](const asio::error_code& ec, size_t) {
            if (g != gen_) return;
            if (ec) return drop();
            watch();
        });
    }

    void drop() {
        asio::error_code ec;
        sock_->close(ec);
        sock_.reset();
        ++gen_;
        std::fprintf(stderr, "data: client disconnected\n");
        deliver_(srv_event(ServerEvent::DataDisconnected, clock_.now()));
    }

    tcp::acceptor acceptor_;
    TransportCfg cfg_;
    Clock& clock_;
    Deliver deliver_;
    std::unique_ptr<tcp::socket> sock_;
    uint32_t gen_ = 0;
    uint8_t scratch_[64];
};

// --dump: writes the Annex B payloads to a file that plays in ffplay.
class FileTx final : public ByteTx {
public:
    FileTx(asio::io_context& io, const std::string& path, Clock& clock, Deliver deliver)
        : io_(io), clock_(clock), deliver_(std::move(deliver)), f_(std::fopen(path.c_str(), "wb")) {
        if (!f_) {
            std::fprintf(stderr, "dump: cannot open %s\n", path.c_str());
            std::exit(1);
        }
        asio::post(io_, [this] { deliver_(srv_event(ServerEvent::DataConnected, clock_.now())); });
    }
    ~FileTx() override { std::fclose(f_); }

    void write(Bytes frame) override {
        std::fwrite(frame.p + kHeaderLen, 1, frame.n - kHeaderLen, f_);
        asio::post(io_, [this] {
            deliver_(srv_event(ServerEvent::WriteDone, clock_.now()));
            deliver_(srv_event(ServerEvent::Writable, clock_.now()));
        });
    }

private:
    asio::io_context& io_;
    Clock& clock_;
    Deliver deliver_;
    std::FILE* f_;
};

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!load_config(cfg, argc, argv)) return 1;
    av_log_set_level(AV_LOG_WARNING);
    const std::string dump = flag(argc, argv, "--dump");
    const std::string seconds = flag(argc, argv, "--seconds");

    asio::io_context io;
    RealClock clock;
    static ServerState state;  // large: keep it off the stack
    server_init(state, cfg);
    CsvLog log(dump.empty() ? cfg.server.csv : "", kServerCsvHeader);

    ServerPorts ports{};
    Deliver deliver = [&](const ServerEvent& ev) { server_dispatch(state, ev, ports); };
    auto post = [&](const ServerEvent& ev) { asio::post(io, [&deliver, ev] { deliver(ev); }); };

    AsioTimers timers(io, kSrvTimerCount, [&](uint8_t id) { deliver(srv_timer(clock.now(), id)); });
    SyntheticSource source;
    AvEncoder encoder(source, clock, encode_params(cfg), post);

    CtrlConn ctrl([&](const CtrlMsg& m) { deliver(srv_ctrl(clock.now(), m)); }, [] {});
    tcp::acceptor ctrl_acceptor(io, tcp::endpoint(tcp::v4(), uint16_t(cfg.transport.control_port)));
    std::function<void()> accept_ctrl = [&] {
        ctrl_acceptor.async_accept([&](const asio::error_code& ec, tcp::socket s) {
            if (!ec) ctrl.attach(std::move(s));
            accept_ctrl();
        });
    };
    accept_ctrl();

    std::unique_ptr<ByteTx> data;
    if (!dump.empty())
        data = std::make_unique<FileTx>(io, dump, clock, deliver);
    else
        data = std::make_unique<TcpDataServer>(io, cfg.transport, clock, deliver);

    ports = ServerPorts{&clock, &timers, data.get(), &ctrl, &encoder, &log};
    std::fprintf(stderr, "h265sim-server: %dx%d@%d, %d kbps, gop %d, data :%d, control :%d%s\n", cfg.source.width,
                 cfg.source.height, cfg.source.fps, cfg.encoder.bitrate_kbps, cfg.encoder.gop,
                 cfg.transport.data_port, cfg.transport.control_port, dump.empty() ? "" : " (dumping)");

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code&, int) { io.stop(); });
    asio::steady_timer stop_timer(io);
    if (!seconds.empty()) {
        stop_timer.expires_after(std::chrono::seconds(std::atoi(seconds.c_str())));
        stop_timer.async_wait([&](const asio::error_code& ec) {
            if (!ec) io.stop();
        });
    }

    post(srv_event(ServerEvent::Start, clock.now()));
    io.run();
    return 0;
}
