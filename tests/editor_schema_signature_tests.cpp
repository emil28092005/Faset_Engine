#include <faset/core/io.hpp>
#include <faset/editor/build_cache.hpp>
#include <faset/editor/session.hpp>
#include <faset/scripting/project.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace faset;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void prepend_fixture_to_path(const fs::path& directory) {
    const auto* previous = std::getenv("PATH");
    const auto value = path_to_utf8(directory) +
#ifdef _WIN32
                       ";" +
#else
                       ":" +
#endif
                       (previous ? previous : "");
#ifdef _WIN32
    check(_putenv_s("PATH", value.c_str()) == 0, "Could not configure fixture PATH");
#else
    check(setenv("PATH", value.c_str(), 1) == 0, "Could not configure fixture PATH");
#endif
}

bool finished(const Json& job) {
    const auto state = job.at("state").get<std::string>();
    return state == "succeeded" || state == "failed" || state == "cancelled";
}
} // namespace

int test_main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() / ("Faset signature race " + new_id());
    try {
        check(argc == 2, "Expected the native build fixture path");
        const auto project = root / "project";
        const auto tools = root / "tools";
        fs::create_directories(tools);
#ifdef _WIN32
        const auto cmake_fixture = tools / "cmake.exe";
#else
        const auto cmake_fixture = tools / "cmake";
#endif
        fs::copy_file(path_from_utf8(argv[1]), cmake_fixture);
        prepend_fixture_to_path(tools);

        {
            editor::Session session({project, path_from_utf8(FASET_TEST_ENGINE), {}});
            session.scaffold("Source signature race", 2);
            atomic_write_json(project / "schema-fixture.json",
                              {{"format", "faset.schema"},
                               {"version", 1},
                               {"types", Json::array()}});
            auto document = session.authoring().create("Export source signature", 2);
            auto& commands = session.commands();

            const auto before =
                editor::gameplay_source_hash(project, scripting::loadLuaProject(project));
            atomic_write(project / "block-native-build", "block\n");
            const auto blocker = commands.call("faset_build", Json::object()).at("job");
            const auto gate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!fs::exists(project / "build-blocked") &&
                   std::chrono::steady_clock::now() < gate_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!fs::exists(project / "build-blocked")) {
                const auto blocked_job = commands.call("faset_job", {{"id", blocker}});
                throw std::runtime_error("The first build never reached the controlled worker "
                                         "gate: " + blocked_job.dump());
            }

            // Submit both requests while the old source exists and another job owns the worker.
            // They must publish the later hash actually captured when their turn begins.
            const auto build = commands.call("faset_build", Json::object()).at("job");
            const auto exported =
                commands.call("faset_export",
                              {{"document", document.at("id")}, {"output", "Exports/Race"}})
                    .at("job");
            atomic_write(project / "Scripts/Gameplay.cpp",
                         read_text(project / "Scripts/Gameplay.cpp") + "\n// queued edit\n");
            const auto after =
                editor::gameplay_source_hash(project, scripting::loadLuaProject(project));
            check(before != after, "Queued source edit did not change the gameplay signature");
            commands.call("faset_job_cancel", {{"id", blocker}});
            fs::remove(project / "block-native-build");

            Json build_job, export_job, blocker_job;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(150);
            do {
                session.poll();
                blocker_job = commands.call("faset_job", {{"id", blocker}});
                build_job = commands.call("faset_job", {{"id", build}});
                export_job = commands.call("faset_job", {{"id", exported}});
                if (finished(blocker_job) && finished(build_job) && finished(export_job))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < deadline);
            session.poll();
            check(blocker_job.at("state") == "cancelled", "Blocked build was not cancelled");
            check(build_job.at("state") == "succeeded",
                  "Queued build failed: " + build_job.value("error", std::string()));
            check(export_job.at("state") == "succeeded",
                  "Queued export failed: " + export_job.value("error", std::string()));

            const auto status = commands.call("faset_schema_status", Json::object());
            check(status.at("loaded") == true && status.at("stale") == false,
                  "Editor marked the just-built source schema stale after a queued edit");
            check(build_job.at("result").value("source_signature", std::string()) == after,
                  "Build result did not report the worker's captured source hash");
            check(export_job.at("result").value("source_signature", std::string()) == after,
                  "Export result did not report the worker's captured source hash");
            check(read_json(project / ".faset/schema-state.json").at("source_signature") == after,
                  "Persisted schema provenance differs from the worker's source snapshot");
        }
        fs::remove_all(root);
        std::cout << "Queued Build and Export preserve the actual worker source signature\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture retained at " << path_to_utf8(root) << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, test_main);
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
