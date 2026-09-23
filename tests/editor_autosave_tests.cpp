#include <faset/authoring/service.hpp>
#include <faset/core/io.hpp>
#include <faset/editor/autosave.hpp>
#include <chrono>
#include <iostream>

using namespace faset;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
void check(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
void contracts(const fs::path& root) {
    authoring::AuthoringService service(root);
    const auto created = service.create("Before", 2);
    const auto id = created.at("id").get<std::string>();
    const auto path = root / "Scenes/main.scene.json";
    service.save(id, "Scenes/main.scene.json");
    int saves{};
    editor::AutosaveController autosave([&](const std::string& document, std::uint64_t revision) {
        ++saves;
        return service.save(document, {}, revision);
    });
    using Clock = editor::AutosaveController::Clock;
    const auto start = Clock::time_point{};
    autosave.observe(service.documents(), start, true);
    for (int index = 1; index <= 3; ++index) {
        const auto revision = service.query(id).at("revision").get<std::uint64_t>();
        service.transact(id, revision,
                         Json::array({{{"op", "scene.rename"},
                                      {"name", "Edit " + std::to_string(index)}}}));
        autosave.observe(service.documents(), start + index * 100ms, true);
    }
    const auto play_snapshot = service.query(id).at("scene");
    autosave.observe(service.documents(), start + 2299ms, true);
    check(saves == 0 && read_json(path).at("name") == "Before",
          "Rapid edits coalesce until the last revision is idle for two seconds");
    autosave.observe(service.documents(), start + 2301ms, true);
    check(saves == 1 && read_json(path).at("name") == "Edit 3" &&
              service.query(id).at("scene") == play_snapshot &&
              autosave.status().at("documents")[0].at("state") == "saved",
          "Named scene saves once without changing the Play snapshot");
    const auto current_revision = service.query(id).at("revision").get<std::uint64_t>();
    check(service.undo(id, current_revision).at("scene").at("name") == "Edit 2",
          "Undo remains available after autosave");
    bool stale_rejected{};
    try {
        service.save(id, {}, current_revision);
    } catch (const Error& error) {
        stale_rejected = error.code() == "revision.conflict";
    }
    check(stale_rejected, "A stale autosave revision cannot save a newer document");

    const auto external = read_text(path) + " \n";
    atomic_write(path, external);
    const auto revision = service.query(id).at("revision").get<std::uint64_t>();
    service.transact(id, revision,
                     Json::array({{{"op", "scene.rename"}, {"name", "After external edit"}}}));
    autosave.observe(service.documents(), start + 3000ms, true);
    autosave.observe(service.documents(), start + 5100ms, true);
    check(saves == 2 && read_text(path) == external &&
              autosave.status().at("documents")[0].at("state") == "conflict",
          "Disk conflict does not overwrite external changes");
    autosave.observe(service.documents(), start + 10000ms, true);
    check(saves == 2, "A failed autosave does not retry every polling frame");

    const auto unnamed = service.create("Unnamed", 2);
    autosave.observe(service.documents(), start + 10100ms, true);
    bool found_unnamed{};
    const auto unnamed_status = autosave.status();
    for (const auto& row : unnamed_status.at("documents"))
        if (row.at("id") == unnamed.at("id"))
            found_unnamed = row.at("state") == "save_as_required";
    check(found_unnamed &&
              fs::is_regular_file(root / ".faset/recovery" /
                                  (unnamed.at("id").get<std::string>() + ".json")),
          "Unnamed scene remains recoverable without receiving an implicit save path");
    autosave.observe(service.documents(), start + 10200ms, false);
    check(!autosave.status().at("enabled").get<bool>() && saves == 2,
          "Disabled autosave leaves recovery journaling active");

    int failed_attempts{};
    editor::AutosaveController failing([&](const std::string&, std::uint64_t) -> Json {
        ++failed_attempts;
        throw Error("io.write", "Synthetic read-only destination");
    });
    Json fake_documents = Json::array(
        {{{"id", "failure-fixture"}, {"revision", 1},
          {"path", "Scenes/fixture.scene.json"}, {"dirty", true}}});
    failing.observe(fake_documents, start, true);
    failing.observe(fake_documents, start + 2100ms, true);
    check(failed_attempts == 1 &&
              failing.status().at("documents")[0].at("state") == "failed",
          "Write failures remain visible");
    failing.observe(fake_documents, start + 10000ms, true);
    check(failed_attempts == 1, "A failed write does not retry on every frame");
    fake_documents[0]["revision"] = 2;
    failing.observe(fake_documents, start + 10100ms, true);
    failing.observe(fake_documents, start + 12200ms, true);
    check(failed_attempts == 2, "A newer revision receives a fresh autosave deadline");
}
} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("faset-autosave-" + new_id());
    try {
        contracts(root);
        fs::remove_all(root);
        std::cout << "Revision-aware autosave contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
