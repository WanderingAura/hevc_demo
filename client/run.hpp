// Client interpreter: runs client_step and executes its commands through the
// ports. Shared by the real runtime and the simulator.
#pragma once
#include "client/core.hpp"
#include "common/ports.hpp"

namespace hs {

struct ClientPorts {
    Clock* clock;
    Timers* timers;
    ByteRx* data;
    MsgTx* ctrl;
    Decoder* decoder;
    LogSink* log;
};

void client_dispatch(ClientState& s, ClientEvent ev, const ClientPorts& p);

}  // namespace hs
