#include "server/run.hpp"

namespace hs {

void server_dispatch(ServerState& s, ServerEvent ev, const ServerPorts& p) {
    if (ev.kind == ServerEvent::Timer) ev.timer.sync = p.clock->sync();
    ServerCmds cmds;
    server_step(s, ev, cmds);
    for (const ServerCmd& c : cmds) {
        switch (c.kind) {
            case ServerCmd::ArmTimer: p.timers->arm(c.arm.id, c.arm.at_mono); break;
            case ServerCmd::StartEncode: {
                uint8_t* slot = slot_ptr(s.pool, c.encode.slot);
                p.encoder->submit(c.encode, MutBytes{slot + kHeaderLen, s.pool.slot_bytes - kHeaderLen});
                break;
            }
            case ServerCmd::Write: {
                uint8_t* frame = slot_ptr(s.pool, c.write.slot);
                patch_send_ts(frame, p.clock->now().wall_ns);  // just before the first write
                p.data->write(Bytes{frame, c.write.len});
                break;
            }
            case ServerCmd::SendCtrl: {
                CtrlMsg m = c.ctrl;
                if (m.type == CtrlType::Pong) m.pong.server_tx_ns = p.clock->now().wall_ns;
                p.ctrl->send(m);
                break;
            }
        }
    }
    while (!s.rows.empty()) p.log->server_row(s.rows.pop());
}

}  // namespace hs
