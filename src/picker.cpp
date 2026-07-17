#include "deck/picker.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace deck {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

void add_recipe(std::vector<ProjectRecipe>& recipes,
                std::string label,
                std::vector<std::string> argv) {
  recipes.push_back({std::move(label), std::move(argv)});
}

}  // namespace

int fuzzy_score(const std::string& query, const std::string& candidate) {
  if (query.empty()) {
    return 1;
  }
  const auto needle = lower(query);
  const auto haystack = lower(candidate);
  if (needle == haystack) {
    return 10000;
  }
  const auto contiguous = haystack.find(needle);
  if (contiguous != std::string::npos) {
    const bool word_start = contiguous == 0 || !std::isalnum(static_cast<unsigned char>(haystack[contiguous - 1]));
    return 7000 - static_cast<int>(contiguous) * 4 + (word_start ? 800 : 0);
  }
  std::size_t position = 0;
  int score = 3000;
  int last = -1;
  for (char ch : needle) {
    const auto found = haystack.find(ch, position);
    if (found == std::string::npos) {
      return -1;
    }
    if (last >= 0) {
      score -= static_cast<int>(found) - last - 1;
    }
    if (found == 0 || !std::isalnum(static_cast<unsigned char>(haystack[found - 1]))) {
      score += 100;
    }
    last = static_cast<int>(found);
    position = found + 1;
  }
  return score - static_cast<int>(haystack.size() / 4);
}

std::vector<ProjectRecipe> discover_project_recipes(const std::filesystem::path& root) {
  std::vector<ProjectRecipe> recipes;
  if (std::filesystem::exists(root / "CMakeLists.txt")) {
    add_recipe(recipes, "CMake: configure", {"cmake", "-S", ".", "-B", "build"});
    add_recipe(recipes, "CMake: build", {"cmake", "--build", "build"});
    add_recipe(recipes, "CMake: test", {"ctest", "--test-dir", "build", "--output-on-failure"});
  }
  if (std::filesystem::exists(root / "pyproject.toml") ||
      std::filesystem::exists(root / "requirements.txt")) {
    add_recipe(recipes, "Python: tests", {"python3", "-m", "pytest"});
  }
  if (std::filesystem::exists(root / "foundry.toml")) {
    add_recipe(recipes, "Foundry: build", {"forge", "build"});
    add_recipe(recipes, "Foundry: test", {"forge", "test"});
  }
  std::ifstream package(root / "package.json");
  if (package) {
    std::ostringstream text;
    text << package.rdbuf();
    const auto source = text.str();
    const auto scripts_at = source.find("\"scripts\"");
    if (scripts_at != std::string::npos) {
      const auto open = source.find('{', scripts_at);
      const auto close = source.find('}', open);
      if (open != std::string::npos && close != std::string::npos) {
        const auto scripts = source.substr(open + 1, close - open - 1);
        const std::regex key(R"re("([A-Za-z0-9:_-]+)"\s*:)re");
        for (auto it = std::sregex_iterator(scripts.begin(), scripts.end(), key);
             it != std::sregex_iterator(); ++it) {
          const auto name = (*it)[1].str();
          add_recipe(recipes, "Node: " + name, {"npm", "run", name});
        }
      }
    }
  }
  return recipes;
}

}  // namespace deck
