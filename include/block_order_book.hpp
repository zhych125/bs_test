#pragma once

#include "order.hpp"

#include <absl/container/flat_hash_map.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// BlockOrderBook stores orders in fixed-size 2KB blocks (64 entries) while allowing
// push/pop on both ends, iterator traversal that skips tombstones, and O(1) random
// removal via an ID -> location index.
class BlockOrderBook {
  struct Block;
  struct Slot;

 public:
  using value_type = Order;
  using reference = value_type&;
  using const_reference = const value_type&;
  using size_type = std::size_t;

  static constexpr std::size_t kBlockCapacity = 64;

  BlockOrderBook() = default;
  BlockOrderBook(const BlockOrderBook& other) { copy_from(other); }
  BlockOrderBook(BlockOrderBook&& other) noexcept { move_from(std::move(other)); }

  BlockOrderBook& operator=(const BlockOrderBook& other) {
    if (this != &other) {
      clear();
      copy_from(other);
    }
    return *this;
  }

  BlockOrderBook& operator=(BlockOrderBook&& other) noexcept {
    if (this != &other) {
      clear();
      move_from(std::move(other));
    }
    return *this;
  }

  ~BlockOrderBook() { clear(); }

  bool empty() const { return size_ == 0; }
  size_type size() const { return size_; }

  void clear() {
    Block* node = head_;
    while (node) {
      for (auto& slot : node->slots) {
        if (slot.live) {
          slot.ptr()->~Order();
          slot.live = false;
        }
      }
      Block* next = node->next;
      delete node;
      node = next;
    }
    head_ = tail_ = nullptr;
    size_ = 0;
    index_.clear();
  }

  reference front() {
    auto it = begin();
    return *it;
  }
  const_reference front() const {
    auto it = begin();
    return *it;
  }

  reference back() {
    auto it = end();
    --it;
    return *it;
  }
  const_reference back() const {
    auto it = end();
    --it;
    return *it;
  }

  void push_back(const Order& order) { emplace_back(order); }
  void push_back(Order&& order) { emplace_back(std::move(order)); }

  template <typename... Args>
  reference emplace_back(Args&&... args) {
    Block* block = ensure_back_block();
    const std::size_t idx = block->end_index;
    block->end_index += 1;
    return construct_slot(block, idx, std::forward<Args>(args)...);
  }

  void push_front(const Order& order) { emplace_front(order); }
  void push_front(Order&& order) { emplace_front(std::move(order)); }

  template <typename... Args>
  reference emplace_front(Args&&... args) {
    Block* block = ensure_front_block();
    block->begin_index -= 1;
    const std::size_t idx = block->begin_index;
    return construct_slot(block, idx, std::forward<Args>(args)...);
  }

  void pop_front() {
    if (empty()) {
      return;
    }
    erase(begin());
  }

  void pop_back() {
    if (empty()) {
      return;
    }
    auto it = end();
    --it;
    erase(it);
  }

  template <bool IsConst>
  class iterator_base {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Order;
    using difference_type = std::ptrdiff_t;
    using reference = std::conditional_t<IsConst, const Order&, Order&>;
    using pointer = std::conditional_t<IsConst, const Order*, Order*>;

    iterator_base() = default;

    template <bool B = IsConst, typename = std::enable_if_t<B>>
    iterator_base(const iterator_base<false>& other)
        : book_(other.book_), block_(other.block_), index_(other.index_) {}

    reference operator*() const { return *book_->slot_order(block_, index_); }
    pointer operator->() const { return &(**this); }

    iterator_base& operator++() {
      book_->advance_forward(block_, index_);
      return *this;
    }
    iterator_base operator++(int) {
      iterator_base tmp = *this;
      ++(*this);
      return tmp;
    }
    iterator_base& operator--() {
      book_->advance_backward(block_, index_);
      return *this;
    }
    iterator_base operator--(int) {
      iterator_base tmp = *this;
      --(*this);
      return tmp;
    }

    friend bool operator==(const iterator_base& lhs, const iterator_base& rhs) {
      return lhs.block_ == rhs.block_ && lhs.index_ == rhs.index_;
    }
    friend bool operator!=(const iterator_base& lhs, const iterator_base& rhs) {
      return !(lhs == rhs);
    }

   private:
    friend class BlockOrderBook;
    using BookPtr = std::conditional_t<IsConst, const BlockOrderBook*, BlockOrderBook*>;

    iterator_base(BookPtr book, Block* block, std::size_t index)
        : book_(book), block_(block), index_(index) {}

    BookPtr book_{nullptr};
    Block* block_{nullptr};
    std::size_t index_{0};
  };

  using iterator = iterator_base<false>;
  using const_iterator = iterator_base<true>;

  iterator begin() {
    auto first = find_next(head_, head_ ? head_->begin_index : 0);
    return iterator(this, first.first, first.second);
  }
  iterator end() { return iterator(this, nullptr, 0); }
  const_iterator begin() const {
    auto first = find_next(head_, head_ ? head_->begin_index : 0);
    return const_iterator(this, first.first, first.second);
  }
  const_iterator end() const { return const_iterator(this, nullptr, 0); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  iterator erase(const_iterator pos) {
    if (pos.block_ == nullptr) {
      return iterator(this, nullptr, 0);
    }
    iterator next(this, pos.block_, pos.index_);
    ++next;
    remove_slot(pos.block_, pos.index_);
    return next;
  }

  iterator erase(iterator pos) { return erase(static_cast<const_iterator>(pos)); }

  bool erase_by_id(std::uint64_t id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
      return false;
    }
    const Location loc = it->second;
    erase(const_iterator(this, loc.block, loc.index));
    return true;
  }

  void copy_range_including_tombstones(const_iterator first,
                                       const_iterator last,
                                       std::vector<Order>& out) const {
    if (first.block_ == nullptr) {
      return;
    }
    std::size_t total = count_slots(first, last);
    if (total == 0) {
      return;
    }
    out.reserve(out.size() + total);
    const Block* block = first.block_;
    std::size_t idx = first.index_;
    while (block) {
      std::size_t end_idx = (block == last.block_) ? last.index_ : block->end_index;
      for (std::size_t i = idx; i < end_idx; ++i) {
        Order tmp;
        std::memcpy(&tmp, block->slots[i].storage, sizeof(Order));
        out.push_back(tmp);
      }
      if (block == last.block_) {
        break;
      }
      block = block->next;
      if (!block) {
        break;
      }
      idx = block->begin_index;
    }
  }

 private:
  struct Slot {
    alignas(Order) unsigned char storage[sizeof(Order)];
    bool live{false};

    Order* ptr() { return std::launder(reinterpret_cast<Order*>(storage)); }
    const Order* ptr() const { return std::launder(reinterpret_cast<const Order*>(storage)); }
  };

  struct Block {
    Block* prev{nullptr};
    Block* next{nullptr};
    std::size_t begin_index{0};
    std::size_t end_index{0};
    std::size_t live_count{0};
    std::array<Slot, kBlockCapacity> slots{};

    bool has_front_space() const { return begin_index > 0; }
    bool has_back_space() const { return end_index < kBlockCapacity; }
  };

  struct Location {
    Block* block{nullptr};
    std::size_t index{0};
  };

  Block* head_{nullptr};
  Block* tail_{nullptr};
  size_type size_{0};
  absl::flat_hash_map<std::uint64_t, Location> index_;

  enum class BlockInit { kCentered, kFront, kBack };

  Block* create_block(BlockInit init) {
    Block* block = new Block();
    switch (init) {
      case BlockInit::kCentered:
        block->begin_index = block->end_index = kBlockCapacity / 2;
        break;
      case BlockInit::kFront:
        block->begin_index = block->end_index = kBlockCapacity;
        break;
      case BlockInit::kBack:
        block->begin_index = block->end_index = 0;
        break;
    }
    return block;
  }

  Block* ensure_back_block() {
    if (!tail_) {
      head_ = tail_ = create_block(BlockInit::kCentered);
    }
    if (!tail_->has_back_space()) {
      Block* block = create_block(BlockInit::kBack);
      link_after(tail_, block);
      tail_ = block;
    }
    return tail_;
  }

  Block* ensure_front_block() {
    if (!head_) {
      head_ = tail_ = create_block(BlockInit::kCentered);
    }
    if (!head_->has_front_space()) {
      Block* block = create_block(BlockInit::kFront);
      link_before(head_, block);
      head_ = block;
    }
    return head_;
  }

  void link_after(Block* node, Block* new_block) {
    new_block->prev = node;
    new_block->next = node ? node->next : nullptr;
    if (node) {
      if (node->next) {
        node->next->prev = new_block;
      }
      node->next = new_block;
    }
    if (tail_ == node) {
      tail_ = new_block;
    }
    if (!head_) {
      head_ = new_block;
    }
  }

  void link_before(Block* node, Block* new_block) {
    new_block->next = node;
    new_block->prev = node ? node->prev : nullptr;
    if (node) {
      if (node->prev) {
        node->prev->next = new_block;
      }
      node->prev = new_block;
    }
    if (head_ == node) {
      head_ = new_block;
    }
    if (!tail_) {
      tail_ = new_block;
    }
  }

  void unlink(Block* block) {
    if (block->prev) {
      block->prev->next = block->next;
    }
    if (block->next) {
      block->next->prev = block->prev;
    }
    if (head_ == block) {
      head_ = block->next;
    }
    if (tail_ == block) {
      tail_ = block->prev;
    }
    delete block;
  }

  template <typename... Args>
  reference construct_slot(Block* block, std::size_t idx, Args&&... args) {
    Slot& slot = block->slots[idx];
    new (slot.storage) Order(std::forward<Args>(args)...);
    slot.live = true;
    block->live_count += 1;
    size_ += 1;
    Order* ptr = slot.ptr();
    index_[ptr->id] = Location{block, idx};
    return *ptr;
  }

  void remove_slot(Block* block, std::size_t idx) {
    Slot& slot = block->slots[idx];
    if (!slot.live) {
      return;
    }
    Order* value = slot.ptr();
    index_.erase(value->id);
    value->~Order();
    slot.live = false;
    block->live_count -= 1;
    size_ -= 1;
    trim_block(block);
  }

  void trim_block(Block* block) {
    while (block->begin_index < block->end_index) {
      if (block->slots[block->begin_index].live) {
        break;
      }
      block->begin_index += 1;
    }
    while (block->end_index > block->begin_index) {
      if (block->slots[block->end_index - 1].live) {
        break;
      }
      block->end_index -= 1;
    }
    if (block->live_count == 0) {
      if (block == head_ && block == tail_) {
        block->begin_index = block->end_index = kBlockCapacity / 2;
      } else {
        unlink(block);
      }
    }
  }

  std::pair<Block*, std::size_t> find_next(Block* block, std::size_t start) const {
    while (block) {
      std::size_t idx = start;
      if (idx < block->begin_index) {
        idx = block->begin_index;
      }
      while (idx < block->end_index) {
        if (block->slots[idx].live) {
          return {block, idx};
        }
        ++idx;
      }
      block = block->next;
      if (block) {
        start = block->begin_index;
      }
    }
    return {nullptr, 0};
  }

  std::pair<Block*, std::size_t> find_prev(Block* block, std::size_t start) const {
    while (block) {
      std::size_t idx = start;
      if (idx > block->end_index) {
        idx = block->end_index;
      }
      while (idx > block->begin_index) {
        idx -= 1;
        if (block->slots[idx].live) {
          return {block, idx};
        }
      }
      block = block->prev;
      if (block) {
        start = block->end_index;
      }
    }
    return {nullptr, 0};
  }

  void advance_forward(Block*& block, std::size_t& index) const {
    if (!block) {
      return;
    }
    auto next = find_next(block, index + 1);
    block = next.first;
    index = next.second;
  }

  void advance_backward(Block*& block, std::size_t& index) const {
    if (!block) {
      auto prev = find_prev(tail_, tail_ ? tail_->end_index : 0);
      block = prev.first;
      index = prev.second;
      return;
    }
    auto prev = find_prev(block, index);
    block = prev.first;
    index = prev.second;
  }

  Order* slot_order(Block* block, std::size_t index) {
    return block ? block->slots[index].ptr() : nullptr;
  }

  const Order* slot_order(Block* block, std::size_t index) const {
    return block ? block->slots[index].ptr() : nullptr;
  }

  std::size_t count_slots(const_iterator first, const_iterator last) const {
    const Block* block = first.block_;
    if (!block) {
      return 0;
    }
    std::size_t idx = first.index_;
    std::size_t total = 0;
    while (block) {
      std::size_t end_idx = (block == last.block_) ? last.index_ : block->end_index;
      if (end_idx > idx) {
        total += end_idx - idx;
      }
      if (block == last.block_) {
        break;
      }
      block = block->next;
      if (!block) {
        break;
      }
      idx = block->begin_index;
    }
    return total;
  }

  void copy_from(const BlockOrderBook& other) {
    for (const auto& order : other) {
      push_back(order);
    }
  }

  void move_from(BlockOrderBook&& other) {
    head_ = other.head_;
    tail_ = other.tail_;
    size_ = other.size_;
    index_ = std::move(other.index_);
    other.head_ = other.tail_ = nullptr;
    other.size_ = 0;
    other.index_.clear();
  }
};
