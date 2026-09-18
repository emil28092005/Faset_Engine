#include "Gameplay.hpp"
#include <faset/core/io.hpp>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        std::filesystem::path output;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "faset_schema_exporter [--output PATH]\nExports declarative gameplay "
                             "schemas without creating a world.\n";
                return 0;
            }
            if (argument == "--output" && i + 1 < argc && output.empty())
                output = argv[++i];
            else
                throw std::invalid_argument("Unknown, repeated or incomplete argument: " +
                                            argument);
        }
        const auto types = faset::gameplay::schema();
        if (!types.is_array())
            throw std::runtime_error("Gameplay schema() must return a type array");
        const nlohmann::json manifest{{"format", "faset.schema"}, {"version", 1}, {"types", types}};
        if (output.empty())
            std::cout << manifest.dump(2) << '\n';
        else
            faset::atomic_write_json(output, manifest);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Schema export failed: " << error.what() << '\n';
        return 1;
    }
}
