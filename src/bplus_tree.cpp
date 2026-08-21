#include "minidb/bplus_tree.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace minidb {

struct BPlusTree::Node {
    explicit Node(bool leaf) : isLeaf(leaf) {}

    bool isLeaf;
    std::vector<IndexKey> keys;
    std::vector<RecordId> values;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;
    Node* next = nullptr;
    Node* previous = nullptr;
};

BPlusTree::BPlusTree(std::size_t leafMaxKeys, std::size_t internalMaxKeys)
    : leafMaxKeys_(leafMaxKeys),
      internalMaxKeys_(internalMaxKeys),
      leafMinKeys_(0),
      internalMinKeys_(0) {
    if (leafMaxKeys < MIN_CONFIGURED_MAX_KEYS
        || internalMaxKeys < MIN_CONFIGURED_MAX_KEYS) {
        throw std::invalid_argument("B+ tree node capacities must each be at least 3.");
    }
    leafMinKeys_ = (leafMaxKeys / 2) + (leafMaxKeys % 2);
    internalMinKeys_ = internalMaxKeys / 2;
}

BPlusTree::~BPlusTree() = default;

bool BPlusTree::insert(IndexKey key, RecordId recordId) {
    if (!recordId.isValid()) {
        throw std::invalid_argument("B+ tree cannot index an invalid RecordId.");
    }

    if (!root_) {
        root_ = std::make_unique<Node>(true);
        root_->keys.push_back(key);
        root_->values.push_back(recordId);
        size_ = 1;
        return true;
    }

    Node* leaf = findLeaf(key);
    const auto position = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (position != leaf->keys.end() && *position == key) {
        return false;
    }

    const auto index = static_cast<std::size_t>(position - leaf->keys.begin());
    leaf->keys.insert(position, key);
    leaf->values.insert(leaf->values.begin() + static_cast<std::ptrdiff_t>(index), recordId);
    ++size_;

    if (leaf->keys.size() > leafMaxKeys_) {
        splitLeaf(leaf);
    } else {
        refreshAncestors(leaf);
    }
    return true;
}

std::optional<RecordId> BPlusTree::find(IndexKey key) const {
    const Node* leaf = findLeaf(key);
    if (!leaf) {
        return std::nullopt;
    }

    const auto position = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (position == leaf->keys.end() || *position != key) {
        return std::nullopt;
    }
    return leaf->values[static_cast<std::size_t>(position - leaf->keys.begin())];
}

std::vector<IndexEntry> BPlusTree::rangeScan(
    IndexKey lowerInclusive,
    IndexKey upperInclusive) const {
    std::vector<IndexEntry> entries;
    if (!root_ || lowerInclusive > upperInclusive) {
        return entries;
    }

    const Node* leaf = findLeaf(lowerInclusive);
    std::size_t index = static_cast<std::size_t>(
        std::lower_bound(leaf->keys.begin(), leaf->keys.end(), lowerInclusive)
        - leaf->keys.begin());

    while (leaf) {
        while (index < leaf->keys.size()) {
            const auto key = leaf->keys[index];
            if (key > upperInclusive) {
                return entries;
            }
            entries.push_back(IndexEntry{key, leaf->values[index]});
            ++index;
        }
        leaf = leaf->next;
        index = 0;
    }
    return entries;
}

std::vector<IndexEntry> BPlusTree::scanAll() const {
    std::vector<IndexEntry> entries;
    entries.reserve(size_);

    const Node* leaf = leftmostLeaf();
    while (leaf) {
        for (std::size_t index = 0; index < leaf->keys.size(); ++index) {
            entries.push_back(IndexEntry{leaf->keys[index], leaf->values[index]});
        }
        leaf = leaf->next;
    }
    return entries;
}

std::size_t BPlusTree::height() const noexcept {
    std::size_t result = 0;
    const Node* node = root_.get();
    while (node) {
        ++result;
        node = node->isLeaf ? nullptr : node->children.front().get();
    }
    return result;
}

BPlusTree::Node* BPlusTree::findLeaf(IndexKey key) noexcept {
    return const_cast<Node*>(std::as_const(*this).findLeaf(key));
}

const BPlusTree::Node* BPlusTree::findLeaf(IndexKey key) const noexcept {
    const Node* node = root_.get();
    while (node && !node->isLeaf) {
        const auto position = std::upper_bound(node->keys.begin(), node->keys.end(), key);
        const auto child = static_cast<std::size_t>(position - node->keys.begin());
        node = node->children[child].get();
    }
    return node;
}

const BPlusTree::Node* BPlusTree::leftmostLeaf() const noexcept {
    const Node* node = root_.get();
    while (node && !node->isLeaf) {
        node = node->children.front().get();
    }
    return node;
}

IndexKey BPlusTree::subtreeMinimum(const Node& node) const {
    const Node* current = &node;
    while (!current->isLeaf) {
        if (current->children.empty()) {
            throw std::logic_error("Internal B+ tree node has no children.");
        }
        current = current->children.front().get();
    }
    if (current->keys.empty()) {
        throw std::logic_error("B+ tree subtree contains an empty leaf.");
    }
    return current->keys.front();
}

std::size_t BPlusTree::childIndex(const Node& parent, const Node* child) const {
    const auto position = std::find_if(
        parent.children.begin(),
        parent.children.end(),
        [child](const auto& candidate) { return candidate.get() == child; });
    if (position == parent.children.end()) {
        throw std::logic_error("B+ tree parent does not own the expected child.");
    }
    return static_cast<std::size_t>(position - parent.children.begin());
}

void BPlusTree::rebuildKeys(Node& internalNode) {
    if (internalNode.isLeaf || internalNode.children.empty()) {
        throw std::logic_error("Cannot rebuild separators for a non-internal node.");
    }

    internalNode.keys.clear();
    internalNode.keys.reserve(internalNode.children.size() - 1);
    for (std::size_t index = 1; index < internalNode.children.size(); ++index) {
        internalNode.keys.push_back(subtreeMinimum(*internalNode.children[index]));
    }
}

void BPlusTree::refreshAncestors(Node* node) {
    Node* current = node ? node->parent : nullptr;
    while (current) {
        rebuildKeys(*current);
        current = current->parent;
    }
}

void BPlusTree::splitLeaf(Node* leaf) {
    auto right = std::make_unique<Node>(true);
    Node* rightPointer = right.get();
    right->parent = leaf->parent;

    const auto leftSize = (leaf->keys.size() + 1) / 2;
    right->keys.assign(leaf->keys.begin() + static_cast<std::ptrdiff_t>(leftSize), leaf->keys.end());
    right->values.assign(
        leaf->values.begin() + static_cast<std::ptrdiff_t>(leftSize),
        leaf->values.end());
    leaf->keys.erase(leaf->keys.begin() + static_cast<std::ptrdiff_t>(leftSize), leaf->keys.end());
    leaf->values.erase(
        leaf->values.begin() + static_cast<std::ptrdiff_t>(leftSize),
        leaf->values.end());

    right->next = leaf->next;
    right->previous = leaf;
    if (right->next) {
        right->next->previous = rightPointer;
    }
    leaf->next = rightPointer;

    insertRightSibling(leaf, std::move(right));
}

void BPlusTree::splitInternal(Node* internalNode) {
    auto right = std::make_unique<Node>(false);
    Node* rightPointer = right.get();
    right->parent = internalNode->parent;

    const auto leftChildCount = (internalNode->children.size() + 1) / 2;
    right->children.reserve(internalNode->children.size() - leftChildCount);
    for (std::size_t index = leftChildCount; index < internalNode->children.size(); ++index) {
        auto child = std::move(internalNode->children[index]);
        child->parent = rightPointer;
        right->children.push_back(std::move(child));
    }
    internalNode->children.erase(
        internalNode->children.begin() + static_cast<std::ptrdiff_t>(leftChildCount),
        internalNode->children.end());

    rebuildKeys(*internalNode);
    rebuildKeys(*right);
    insertRightSibling(internalNode, std::move(right));
}

void BPlusTree::insertRightSibling(Node* left, std::unique_ptr<Node> right) {
    if (left == root_.get()) {
        auto newRoot = std::make_unique<Node>(false);
        auto oldRoot = std::move(root_);
        oldRoot->parent = newRoot.get();
        right->parent = newRoot.get();
        newRoot->children.push_back(std::move(oldRoot));
        newRoot->children.push_back(std::move(right));
        rebuildKeys(*newRoot);
        root_ = std::move(newRoot);
        return;
    }

    Node* parent = left->parent;
    const auto index = childIndex(*parent, left);
    right->parent = parent;
    parent->children.insert(
        parent->children.begin() + static_cast<std::ptrdiff_t>(index + 1),
        std::move(right));
    rebuildKeys(*parent);

    if (parent->keys.size() > internalMaxKeys_) {
        splitInternal(parent);
    } else {
        refreshAncestors(parent);
    }
}

void BPlusTree::validate() const {
    if (!root_) {
        if (size_ != 0) {
            throw std::runtime_error("Empty B+ tree has a nonzero size.");
        }
        return;
    }
    if (root_->parent != nullptr) {
        throw std::runtime_error("B+ tree root has a parent.");
    }

    struct Bounds {
        IndexKey minimum;
        IndexKey maximum;
        std::size_t entries;
    };

    std::optional<std::size_t> leafDepth;
    std::vector<const Node*> leaves;

    std::function<Bounds(const Node&, const Node*, std::size_t)> validateNode;
    validateNode = [&](const Node& node, const Node* expectedParent, std::size_t depth) -> Bounds {
        if (node.parent != expectedParent) {
            throw std::runtime_error("B+ tree child has an incorrect parent pointer.");
        }
        if (!std::is_sorted(node.keys.begin(), node.keys.end())
            || std::adjacent_find(node.keys.begin(), node.keys.end()) != node.keys.end()) {
            throw std::runtime_error("B+ tree node keys are not strictly sorted.");
        }

        const bool isRoot = &node == root_.get();
        if (node.isLeaf) {
            if (!node.children.empty()) {
                throw std::runtime_error("B+ tree leaf owns child nodes.");
            }
            if (node.keys.size() != node.values.size()) {
                throw std::runtime_error("B+ tree leaf key/value counts differ.");
            }
            if (node.keys.empty() || node.keys.size() > leafMaxKeys_) {
                throw std::runtime_error("B+ tree leaf has illegal occupancy.");
            }
            if (!isRoot && node.keys.size() < leafMinKeys_) {
                throw std::runtime_error("Non-root B+ tree leaf is below minimum occupancy.");
            }
            for (const auto& value : node.values) {
                if (!value.isValid()) {
                    throw std::runtime_error("B+ tree leaf contains an invalid RecordId.");
                }
            }
            if (!leafDepth) {
                leafDepth = depth;
            } else if (*leafDepth != depth) {
                throw std::runtime_error("B+ tree leaves are at different depths.");
            }
            leaves.push_back(&node);
            return Bounds{node.keys.front(), node.keys.back(), node.keys.size()};
        }

        if (!node.values.empty() || node.next != nullptr || node.previous != nullptr) {
            throw std::runtime_error("B+ tree internal node contains leaf-only state.");
        }
        if (node.children.size() != node.keys.size() + 1 || node.children.empty()) {
            throw std::runtime_error("B+ tree internal child-count invariant failed.");
        }
        if (node.keys.size() > internalMaxKeys_) {
            throw std::runtime_error("B+ tree internal node exceeds maximum occupancy.");
        }
        if (isRoot) {
            if (node.keys.empty()) {
                throw std::runtime_error("B+ tree internal root has fewer than two children.");
            }
        } else if (node.keys.size() < internalMinKeys_) {
            throw std::runtime_error("Non-root B+ tree internal node is below minimum occupancy.");
        }

        std::vector<Bounds> childBounds;
        childBounds.reserve(node.children.size());
        for (const auto& child : node.children) {
            if (!child) {
                throw std::runtime_error("B+ tree internal node owns a null child.");
            }
            childBounds.push_back(validateNode(*child, &node, depth + 1));
        }

        std::size_t entries = childBounds.front().entries;
        for (std::size_t index = 1; index < childBounds.size(); ++index) {
            if (childBounds[index - 1].maximum >= childBounds[index].minimum) {
                throw std::runtime_error("B+ tree child key ranges overlap.");
            }
            if (node.keys[index - 1] != childBounds[index].minimum) {
                throw std::runtime_error("B+ tree separator does not equal right-subtree minimum.");
            }
            entries += childBounds[index].entries;
        }
        return Bounds{childBounds.front().minimum, childBounds.back().maximum, entries};
    };

    const auto bounds = validateNode(*root_, nullptr, 0);
    if (bounds.entries != size_) {
        throw std::runtime_error("B+ tree entry count does not match size.");
    }

    for (std::size_t index = 0; index < leaves.size(); ++index) {
        const Node* expectedPrevious = index == 0 ? nullptr : leaves[index - 1];
        const Node* expectedNext = index + 1 == leaves.size() ? nullptr : leaves[index + 1];
        if (leaves[index]->previous != expectedPrevious || leaves[index]->next != expectedNext) {
            throw std::runtime_error("B+ tree leaf links disagree with tree order.");
        }
        if (expectedPrevious && expectedPrevious->keys.back() >= leaves[index]->keys.front()) {
            throw std::runtime_error("Adjacent B+ tree leaves are not strictly ordered.");
        }
    }

    const Node* forward = leaves.front();
    for (const Node* expected : leaves) {
        if (forward != expected) {
            throw std::runtime_error("B+ tree forward leaf chain skips or duplicates a leaf.");
        }
        forward = forward->next;
    }
    if (forward != nullptr) {
        throw std::runtime_error("B+ tree forward leaf chain contains extra nodes or a cycle.");
    }

    const Node* backward = leaves.back();
    for (auto position = leaves.rbegin(); position != leaves.rend(); ++position) {
        if (backward != *position) {
            throw std::runtime_error("B+ tree backward leaf chain skips or duplicates a leaf.");
        }
        backward = backward->previous;
    }
    if (backward != nullptr) {
        throw std::runtime_error("B+ tree backward leaf chain contains extra nodes or a cycle.");
    }
}

} // namespace minidb
