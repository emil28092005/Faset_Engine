#pragma once
#include <functional>
#include <string>
#include <vector>
namespace faset::render {
// Ordered single-queue graph. Reads must be imported or produced by an earlier pass.
// The Vulkan executor performs barriers at each resource state transition.
class RenderGraph {
  public:
    using Callback = std::function<void()>;
    void import(std::string resource);
    void add(std::string name, std::vector<std::string> reads, std::vector<std::string> writes,
             Callback execute);
    void execute() const;
    std::vector<std::string> pass_names() const;

  private:
    struct Pass {
        std::string name;
        std::vector<std::string> reads, writes;
        Callback callback;
    };
    std::vector<std::string> imports_;
    std::vector<Pass> passes_;
};
} // namespace faset::render
