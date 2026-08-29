#pragma once
#include <cstddef>
#include <vector>

// Set Signature Tree per Definition 10 (Gosselin et al., TCS 2011).
// Stores Word Signatures (vector<int>) as paths from root to leaf.
// Each node maps label → child; all children of a node have distinct labels.
// Search/insert is O(word_length × branching_factor).
// Branching factor is small in practice (avg < 4 for 10K-map databases).

class SigTree {
public:
    SigTree();
    ~SigTree();

    // Insert a word into the tree.  Returns true if it was new.
    bool insert(const std::vector<int>& word);

    // Check if a word exists in the tree (exact path match).
    bool contains(const std::vector<int>& word) const;

    // Number of stored words (leaf count).
    size_t size() const { return leaf_count_; }

    // Total number of nodes in the tree (for memory measurement).
    size_t node_count() const;

private:
    struct Node {
        int label;                     // -1 for root
        std::vector<Node*> children;   // sorted by label
        bool is_leaf;                  // true if a word ends here

        Node(int lbl) : label(lbl), is_leaf(false) {}
        ~Node();

        // Find child with given label, or nullptr
        Node* find_child(int lbl) const;
        // Find or create child with given label
        Node* get_or_create(int lbl);
    };

    Node root_;
    size_t leaf_count_ = 0;
};
