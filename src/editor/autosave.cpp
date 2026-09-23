#include <faset/editor/autosave.hpp>

#include <faset/core/error.hpp>
#include <set>
#include <stdexcept>

namespace faset::editor {

AutosaveController::AutosaveController(SaveFn save) : save_(std::move(save)) {
    if (!save_)
        throw std::invalid_argument("Autosave requires a save callback");
}

void AutosaveController::observe(const Json& documents, Clock::time_point now, bool enabled) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    std::set<std::string> present;
    for (const auto& document : documents) {
        const auto id = document.at("id").get<std::string>();
        const auto revision = document.at("revision").get<std::uint64_t>();
        const auto path = document.value("path", std::string());
        const auto dirty = document.value("dirty", false);
        present.insert(id);
        auto [found, inserted] = entries_.try_emplace(id);
        auto& entry = found->second;
        const auto changed = inserted || entry.revision != revision || entry.path != path;
        const auto prior_state = entry.state;
        entry.revision = revision;
        entry.path = path;
        if (!dirty) {
            entry.state = "saved";
            entry.error.clear();
            continue;
        }
        if (path.empty()) {
            entry.state = "save_as_required";
            entry.error.clear();
            continue;
        }
        if (!enabled) {
            entry.state = "disabled";
            entry.error.clear();
            continue;
        }
        if (changed || prior_state == "disabled" || prior_state == "saved" ||
            prior_state == "save_as_required") {
            entry.state = "pending";
            entry.error.clear();
            entry.deadline = now + std::chrono::seconds(2);
        }
        if (entry.state != "pending" || now < entry.deadline)
            continue;
        entry.state = "saving";
        try {
            const auto saved = save_(id, revision);
            if (saved.at("revision").get<std::uint64_t>() != revision ||
                saved.at("dirty").get<bool>())
                throw std::runtime_error("Save returned an unexpected document revision");
            entry.state = "saved";
            entry.error.clear();
        } catch (const Error& error) {
            entry.state = error.code() == "save.disk_conflict" ||
                                  error.code() == "revision.conflict"
                              ? "conflict"
                              : "failed";
            entry.error = error.what();
        } catch (const std::exception& error) {
            entry.state = "failed";
            entry.error = error.what();
        }
    }
    for (auto it = entries_.begin(); it != entries_.end();)
        it = present.contains(it->first) ? std::next(it) : entries_.erase(it);
}

Json AutosaveController::status() const {
    std::lock_guard lock(mutex_);
    Json documents = Json::array();
    for (const auto& [id, entry] : entries_)
        documents.push_back({{"id", id},
                             {"state", entry.state},
                             {"revision", entry.revision},
                             {"path", entry.path},
                             {"error", entry.error}});
    return {{"enabled", enabled_}, {"documents", documents}};
}

} // namespace faset::editor
