#pragma once
#include <string>
#include <vector>

namespace catalog {

constexpr int NUM_VERTEX_TYPES = 44;

extern const std::vector<std::string> symbols;
extern const std::vector<std::vector<std::string>> edge_label_templates;
extern const std::vector<std::vector<int>> left_neighbors;
extern const std::vector<std::vector<int>> right_neighbors;
extern const std::vector<std::vector<int>> mirrors;
extern const std::vector<std::vector<int>> polygon_sizes;
extern const std::vector<std::string> codes;

int attachment_limit(int vertex_type);

} // namespace catalog
