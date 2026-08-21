#pragma once

#include "minidb/index_types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace minidb {

class BPlusTree {
public:
    static constexpr std::size_t DEFAULT_LEAF_MAX_KEYS = 64;
    static constexpr std::size_t DEFAULT_INTERNAL_MAX_KEYS = 64;
    static constexpr std::size_t MIN_CONFIGURED_MAX_KEYS = 3;

    explicit BPlusTree(
        std::size_t leafMaxKeys = DEFAULT_LEAF_MAX_KEYS,
        std::size_t internalMaxKeys = DEFAULT_INTERNAL_MAX_KEYS);
    ~BPlusTree();

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    [[nodiscard]] bool insert(IndexKey key, RecordId recordId);
    [[nodiscard]] bool erase(IndexKey key);
    [[nodiscard]] std::optional<RecordId> find(IndexKey key) const;
    [[nodiscard]] std::vector<IndexEntry> rangeScan(
        IndexKey lowerInclusive,
        IndexKey upperInclusive) const;
    [[nodiscard]] std::vector<IndexEntry> scanAll() const;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t height() const noexcept;

    [[nodiscard]] std::size_t leafMaxKeys() const noexcept { return leafMaxKeys_; }
    [[nodiscard]] std::size_t internalMaxKeys() const noexcept {
        return internalMaxKeys_;
    }
    [[nodiscard]] std::size_t leafMinKeys() const noexcept { return leafMinKeys_; }
    [[nodiscard]] std::size_t internalMinKeys() const noexcept {
        return internalMinKeys_;
    }

    void validate() const;

private:
    struct Node;

    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
    std::size_t leafMaxKeys_;
    std::size_t internalMaxKeys_;
    std::size_t leafMinKeys_;
    std::size_t internalMinKeys_;

    [[nodiscard]] Node* findLeaf(IndexKey key) noexcept;
    [[nodiscard]] const Node* findLeaf(IndexKey key) const noexcept;
    [[nodiscard]] const Node* leftmostLeaf() const noexcept;
    [[nodiscard]] IndexKey subtreeMinimum(const Node& node) const;
    [[nodiscard]] std::size_t childIndex(const Node& parent, const Node* child) const;

    void rebuildKeys(Node& internalNode);
    void refreshAncestors(Node* node);
    void splitLeaf(Node* leaf);
    void splitInternal(Node* internalNode);
    void insertRightSibling(Node* left, std::unique_ptr<Node> right);
    void rebalanceLeaf(Node* leaf);
    void rebalanceInternal(Node* internalNode);
    void finishInternalRemoval(Node* internalNode);
};

} // namespace minidb
