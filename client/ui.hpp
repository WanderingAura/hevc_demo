// Client UI: SDL3 window with the latest frame, ImGui controls, ImPlot charts.
// Runs on the main thread (SDL requires it); talks to the logic thread only
// through the frame mailbox, UiFeed, and posted UI events.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "client/decoder_av.hpp"

namespace hs {

// LogSink that hands rows and the status snapshot to the UI thread.
class UiFeed final : public LogSink {
public:
    UiFeed();
    void client_row(const ClientRow& r) override;
    void client_status(const ClientStatus& s) override;
    // UI thread: swaps the pending rows into `rows` and copies the status.
    void take(std::vector<ClientRow>& rows, ClientStatus& status);

private:
    std::mutex mu_;
    std::vector<ClientRow> rows_;
    ClientStatus status_{};
};

using PostEvent = std::function<void(const ClientEvent&)>;

// Returns when the window is closed or `quit` becomes true.
void run_ui(const Config& cfg, FrameMailbox& mailbox, UiFeed& feed, Clock& clock, const PostEvent& post,
            const std::atomic<bool>& quit);

}  // namespace hs
