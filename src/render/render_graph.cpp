#include <faset/render/render_graph.hpp>
#include <stdexcept>
#include <unordered_set>
#include <utility>
namespace faset::render {
void RenderGraph::import(std::string resource) { imports_.push_back(std::move(resource)); }
void RenderGraph::add(std::string name, std::vector<std::string> reads, std::vector<std::string> writes, Callback execute) {
    if (name.empty() || !execute) throw std::invalid_argument("RenderGraph pass requires a name and callback");
    for (const auto& pass : passes_) if (pass.name == name) throw std::invalid_argument("Duplicate RenderGraph pass: " + name);
    passes_.push_back({std::move(name),std::move(reads),std::move(writes),std::move(execute)});
}
void RenderGraph::execute() const {
    std::unordered_set<std::string> available(imports_.begin(), imports_.end());
    // Validate the whole graph before recording any GPU work.
    for (const auto& pass : passes_) {
        for (const auto& resource : pass.reads)
            if (!available.contains(resource)) throw std::runtime_error("RenderGraph pass '" + pass.name + "' reads uninitialized resource '" + resource + "'");
        for (const auto& resource : pass.writes) available.insert(resource);
    }
    for (const auto& pass : passes_) pass.callback();
}
std::vector<std::string> RenderGraph::pass_names() const {
    std::vector<std::string> result;
    for (const auto& pass : passes_) result.push_back(pass.name);
    return result;
}
}
