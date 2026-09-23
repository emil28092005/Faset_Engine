#pragma once
#include <faset/render/renderer.hpp>
#include <memory>
#include <span>

namespace faset::editor {
// Optional developer diagnostics. This module never edits authoring or runtime state.
class DebugOverlay {
  public:
    DebugOverlay();
    ~DebugOverlay();
    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    bool visible() const;
    void set_visible(bool);
    // F12 toggles the overlay; events captured by its widgets are removed for this frame.
    std::vector<render::Event> process_events(std::span<const render::Event>);
    void append(render::Snapshot&, render::Renderer&, float delta_seconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace faset::editor
