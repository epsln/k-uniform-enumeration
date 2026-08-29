#include "sig_tree.h"
#include <algorithm>
#include <functional>

// =============================================================================
// Node
// =============================================================================

SigTree::Node::~Node() {
    for (auto* child : children) delete child;
}

SigTree::Node* SigTree::Node::find_child(int lbl) const {
    // Linear search — branching factor is small in practice
    for (auto* c : children)
        if (c->label == lbl) return c;
    return nullptr;
}

SigTree::Node* SigTree::Node::get_or_create(int lbl) {
    // Find insertion point (keep sorted by label)
    auto it = std::lower_bound(children.begin(), children.end(), lbl,
        [](const Node* n, int l) { return n->label < l; });
    if (it != children.end() && (*it)->label == lbl)
        return *it;
    auto* child = new Node(lbl);
    children.insert(it, child);
    return child;
}

// =============================================================================
// SigTree
// =============================================================================

SigTree::SigTree() : root_(-1) {}
SigTree::~SigTree() = default;  // root_ destructor handles children

bool SigTree::insert(const std::vector<int>& word) {
    Node* node = &root_;
    for (int lbl : word) {
        node = node->get_or_create(lbl);
    }
    if (node->is_leaf) return false;
    node->is_leaf = true;
    ++leaf_count_;
    return true;
}

bool SigTree::contains(const std::vector<int>& word) const {
    const Node* node = &root_;
    for (int lbl : word) {
        node = node->find_child(lbl);
        if (!node) return false;
    }
    return node->is_leaf;
}

size_t SigTree::node_count() const {
    std::function<size_t(const Node*)> count = [&](const Node* n) -> size_t {
        size_t c = 1;
        for (auto* child : n->children) c += count(child);
        return c;
    };
    return count(&root_);
}
