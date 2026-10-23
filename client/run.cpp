#include "client/run.hpp"

namespace hs {

void client_dispatch(ClientState& s, ClientEvent ev, const ClientPorts& p) {
    if (ev.kind == ClientEvent::Timer) ev.timer.sync = p.clock->sync();
    ClientCmds cmds;
    client_step(s, ev, cmds);
    for (const ClientCmd& c : cmds) {
        switch (c.kind) {
            case ClientCmd::ArmTimer: p.timers->arm(c.arm.id, c.arm.at_mono); break;
            case ClientCmd::StartDecode:
                p.decoder->submit(c.decode, Bytes{frame_ptr(s.pool, c.decode.slot), s.pool.hdr[c.decode.slot].payload_len});
                break;
            case ClientCmd::Present: p.decoder->present(c.present_seq); break;
            case ClientCmd::SendCtrl: p.ctrl->send(c.ctrl); break;
            case ClientCmd::CloseData: p.data->close(); break;
        }
    }
    while (!s.rows.empty()) p.log->client_row(s.rows.pop());
    p.log->client_status(s.status);
}

}  // namespace hs
