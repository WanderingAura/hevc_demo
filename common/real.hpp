// Real implementations of the shared ports: system clocks, Asio timers, CSV
// logging and the TCP control connection. Used by both binaries, never by tests
// of the cores.
#pragma once
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "common/config.hpp"
#include "common/ports.hpp"

namespace hs {

struct RealClock final : Clock {
    Time now() override;
    SyncStatus sync() override;  // ntp_adjtime(), read-only
};

class AsioTimers final : public Timers {
public:
    using Fire = std::function<void(uint8_t id)>;
    AsioTimers(asio::io_context& io, int count, Fire fire);
    void arm(uint8_t id, int64_t at_mono_ns) override;

private:
    std::vector<std::unique_ptr<asio::steady_timer>> timers_;
    Fire fire_;
};

class CsvLog final : public LogSink {
public:
    CsvLog(const std::string& path, const char* header);  // empty path: disabled
    ~CsvLog() override;
    void server_row(const ServerRow& r) override;
    void client_row(const ClientRow& r) override;

private:
    std::FILE* f_ = nullptr;
};

// Control channel over one TCP socket (either side). Reads CtrlMsgs, sends
// through an outbox so a slow peer never blocks the loop.
class CtrlConn final : public MsgTx {
public:
    using OnMsg = std::function<void(const CtrlMsg&)>;
    using OnClose = std::function<void()>;
    CtrlConn(OnMsg on_msg, OnClose on_close);
    void attach(asio::ip::tcp::socket s);
    void close();
    bool connected() const { return sock_ != nullptr; }
    void send(const CtrlMsg& m) override;

private:
    void read();
    void flush();

    OnMsg on_msg_;
    OnClose on_close_;
    std::unique_ptr<asio::ip::tcp::socket> sock_;
    uint32_t gen_ = 0;
    CtrlReader reader_{};
    uint8_t rx_[4096];
    std::vector<uint8_t> outbox_, inflight_;
    bool writing_ = false;
};

// TCP_NODELAY, small SO_SNDBUF, TCP_NOTSENT_LOWAT.
void tune_socket(asio::ip::tcp::socket& s, const TransportCfg& t);

// Loads the INI file (first arg without '=' or leading "--") and applies
// `section.key=value` overrides. Prints errors to stderr.
bool load_config(Config& c, int argc, char** argv);
// "--name=value" -> value, or "" if absent. "--name" alone -> "1".
std::string flag(int argc, char** argv, const char* name);

}  // namespace hs
