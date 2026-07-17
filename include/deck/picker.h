#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace deck {

struct ProjectRecipe {
  std::string label;
  std::vector<std::string> argv;
};

int fuzzy_score(const std::string& query, const std::string& candidate);
std::vector<ProjectRecipe> discover_project_recipes(const std::filesystem::path& root);

}  // namespace deck
