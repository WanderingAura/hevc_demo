// Server interpreter: runs server_step and executes its commands through the
// ports. Shared by the real runtime and the simulator.
#pragma once
#include "common/ports.hpp"
#include "server/core.hpp"

namespace hs {

struct ServerPorts {
    Clock* clock;
    Timers* timers;
    ByteTx* data;
    MsgTx* ctrl;
    Encoder* encoder;
    LogSink* log;
};

void server_dispatch(ServerState& s, ServerEvent ev, const ServerPorts& p);

}  // namespace hs
