#pragma once
#include <faset/editor/session.hpp>
#include <faset/ui/ui.hpp>
#include <memory>

namespace faset::editor {
// Owns authoring presentation only. Player execution and MCP transport stay in
// the application.
class EditorUI {
  public:
    EditorUI(Session&, render::Renderer&, const std::filesystem::path& font,
             const std::filesystem::path& styles);
    ~EditorUI();
    EditorUI(const EditorUI&) = delete;
    EditorUI& operator=(const EditorUI&) = delete;
    void frame(const std::vector<render::Event>&);
    const render::Snapshot& snapshot() const;
    ui::Context& widgets();
    const std::string& current_document() const;
    void select_document(const std::string& document);
    const std::string& selected_entity() const;
    void select_entity(const std::string& entity);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace faset::editor
