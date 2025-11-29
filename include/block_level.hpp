#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "block.hpp"

template <typename T, std::size_t BlockCapacity = 64>
class VolumeBreakdown {
  static_assert(std::is_convertible_v<decltype(std::declval<T&>().id), std::uint64_t>,
                "VolumeBreakdown requires value_type.id convertible to uint64_t");

  using BlockType = Block<T, BlockCapacity>;

 public:
  using value_type = T;
  using size_type = std::size_t;

  VolumeBreakdown() = default;
  VolumeBreakdown(const VolumeBreakdown&) = delete;
  VolumeBreakdown& operator=(const VolumeBreakdown&) = delete;

  VolumeBreakdown(VolumeBreakdown&& other) noexcept { move_from(std::move(other)); }
  VolumeBreakdown& operator=(VolumeBreakdown&& other) noexcept {
    if (this != &other) {
      clear();
      move_from(std::move(other));
    }
    return *this;
  }

  ~VolumeBreakdown() { clear(); }

  bool empty() const { return size_ == 0; }
  size_type size() const { return size_; }

  void clear() {
    BlockType* block = head_;
    while (block) {
      BlockType* next = block->next();
      delete block;
      block = next;
    }
    head_ = tail_ = nullptr;
    size_ = 0;
    block_count_ = 0;
    deactivate_index();
  }

  value_type& front() {
    assert(!empty());
    return head_->front();
  }
  const value_type& front() const {
    assert(!empty());
    return head_->front();
  }

  value_type& back() {
    assert(!empty());
    return tail_->back();
  }
  const value_type& back() const {
    assert(!empty());
    return tail_->back();
  }

  void push_back(const value_type& value) { emplace_back(value); }
  void push_back(value_type&& value) { emplace_back(std::move(value)); }

  template <typename... Args>
  value_type& emplace_back(Args&&... args) {
    BlockType* block = ensure_tail_block();
    value_type& result = block->emplace_back(std::forward<Args>(args)...);
    ++size_;
    on_insert(block, result);
    return result;
  }

  void push_front(const value_type& value) { emplace_front(value); }
  void push_front(value_type&& value) { emplace_front(std::move(value)); }

  template <typename... Args>
  value_type& emplace_front(Args&&... args) {
    BlockType* block = ensure_head_block();
    value_type& result = block->emplace_front(std::forward<Args>(args)...);
    ++size_;
    on_insert(block, result);
    return result;
  }

  void pop_back() {
    assert(!empty());
    BlockType* block = tail_;
    const std::uint64_t id = block->back().id;
    block->pop_back();
    --size_;
    on_remove(id);
    if (block->empty()) {
      remove_block(block);
    }
  }

  void pop_front() {
    assert(!empty());
    BlockType* block = head_;
    const std::uint64_t id = block->front().id;
    block->pop_front();
    --size_;
    on_remove(id);
    if (block->empty()) {
      remove_block(block);
    }
  }

  template <bool IsConst>
  class iterator_base {
    using owner_pointer =
        std::conditional_t<IsConst, const VolumeBreakdown*, VolumeBreakdown*>;
    using block_pointer = std::conditional_t<IsConst, const BlockType*, BlockType*>;
    using reference = std::conditional_t<IsConst, const value_type&, value_type&>;
    using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;

   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = VolumeBreakdown::value_type;
    using difference_type = std::ptrdiff_t;

    iterator_base() = default;
    iterator_base(owner_pointer owner, block_pointer block, size_type index)
        : owner_(owner), block_(block), index_(index) {}

    template <bool B = IsConst, typename = std::enable_if_t<B>>
    iterator_base(const iterator_base<false>& other)
        : owner_(other.owner_), block_(other.block_), index_(other.index_) {}

    reference operator*() const { return (*block_)[index_]; }
    pointer operator->() const { return &(*block_)[index_]; }

    iterator_base& operator++() {
      advance_forward();
      return *this;
    }
    iterator_base operator++(int) {
      iterator_base tmp = *this;
      ++(*this);
      return tmp;
    }

    iterator_base& operator--() {
      advance_backward();
      return *this;
    }
    iterator_base operator--(int) {
      iterator_base tmp = *this;
      --(*this);
      return tmp;
    }

    friend bool operator==(const iterator_base& a, const iterator_base& b) {
      return a.block_ == b.block_ && a.index_ == b.index_;
    }
    friend bool operator!=(const iterator_base& a, const iterator_base& b) { return !(a == b); }

   private:
    void advance_forward() {
      if (!block_) return;
      ++index_;
      if (index_ < block_->size()) return;
      block_ = block_->next();
      index_ = 0;
    }

    void advance_backward() {
      if (!owner_) return;
      if (!block_) {
        block_ = owner_->tail_;
        if (!block_) return;
        index_ = block_->size() - 1;
        return;
      }
      if (index_ > 0) {
        --index_;
        return;
      }
      block_ = block_->prev();
      if (!block_) {
        index_ = 0;
        return;
      }
      index_ = block_->size() - 1;
    }

    owner_pointer owner_{nullptr};
    block_pointer block_{nullptr};
    size_type index_{0};

    friend class VolumeBreakdown;
  };

  using iterator = iterator_base<false>;
  using const_iterator = iterator_base<true>;

  iterator erase(const_iterator pos) {
    if (!pos.block_) {
      return end();
    }
    return erase_at(pos.block_, pos.index_);
  }

  iterator erase(iterator pos) { return erase(static_cast<const_iterator>(pos)); }

  bool erase_by_id(std::uint64_t id) {
    auto loc = locate_by_id(id);
    if (!loc.block) {
      return false;
    }
    erase_at(loc.block, loc.index);
    return true;
  }

  iterator begin() { return iterator(this, head_, 0); }
  iterator end() { return iterator(this, nullptr, 0); }
  const_iterator begin() const { return const_iterator(this, head_, 0); }
  const_iterator end() const { return const_iterator(this, nullptr, 0); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  iterator find(std::uint64_t id) {
    auto loc = locate_by_id(id);
    if (!loc.block) {
      return end();
    }
    return iterator(this, loc.block, loc.index);
  }

  const_iterator find(std::uint64_t id) const {
    auto loc = locate_by_id(id);
    if (!loc.block) {
      return end();
    }
    return const_iterator(this, loc.block, loc.index);
  }

  std::pair<const_iterator, const_iterator> volume_range(std::int64_t lower,
                                                         std::int64_t upper) const {
    if (lower <= 0) {
      lower = 1;
    }
    if (upper < lower) {
      upper = lower;
    }
    const std::int64_t end_target =
        (upper == std::numeric_limits<std::int64_t>::max()) ? upper : upper + 1;
    auto start = find_position_by_volume(lower);
    auto finish = find_position_by_volume(end_target);
    return {const_iterator(this, start.first, start.second),
            const_iterator(this, finish.first, finish.second)};
  }

 private:
  iterator erase_at(BlockType* block, size_type index) {
    const std::uint64_t id = (*block)[index].id;
    block->erase(index);
    --size_;
    on_remove(id);
    BlockType* next_block = block;
    size_type next_index = index;
    if (block->empty()) {
      next_block = block->next();
      remove_block(block);
    } else if (next_index >= block->size()) {
      next_block = block->next();
      next_index = 0;
    }
    if (!next_block) {
      next_index = 0;
    }
    return iterator(this, next_block, next_index);
  }

  BlockType* ensure_head_block() {
    if (!head_) {
      BlockType* block = create_block();
      head_ = tail_ = block;
      return block;
    }
    if (head_->full()) {
      BlockType* block = create_block();
      block->set_next(head_);
      head_->set_prev(block);
      head_ = block;
      return block;
    }
    return head_;
  }

  BlockType* ensure_tail_block() {
    if (!tail_) {
      BlockType* block = create_block();
      head_ = tail_ = block;
      return block;
    }
    if (tail_->full()) {
      BlockType* block = create_block();
      block->set_prev(tail_);
      tail_->set_next(block);
      tail_ = block;
      return block;
    }
    return tail_;
  }

  BlockType* create_block() {
    BlockType* block = new BlockType();
    ++block_count_;
    activate_index_if_needed();
    return block;
  }

  void remove_block(BlockType* block) {
    BlockType* prev = block->prev();
    BlockType* next = block->next();
    if (prev) {
      prev->set_next(next);
    } else {
      head_ = next;
    }
    if (next) {
      next->set_prev(prev);
    } else {
      tail_ = prev;
    }
    delete block;
    --block_count_;
    if (block_count_ <= 1) {
      deactivate_index();
    }
  }

  struct Location {
    BlockType* block{nullptr};
    size_type index{0};
  };

  Location locate_by_id(std::uint64_t id) const {
    if (index_active_) {
      auto it = block_index_.find(id);
      if (it == block_index_.end()) {
        return {};
      }
      return locate_within_block(it->second, id);
    }
    return locate_within_block(head_, id);
  }

  Location locate_within_block(BlockType* block, std::uint64_t id) const {
    if (!block) {
      return {};
    }
    for (size_type i = 0; i < block->size(); ++i) {
      if ((*block)[i].id == id) {
        return Location{block, i};
      }
    }
    return {};
  }

  std::pair<BlockType*, size_type> find_position_by_volume(std::int64_t target) const {
    if (target <= 0) {
      return {head_, 0};
    }
    BlockType* block = head_;
    std::int64_t accumulated = 0;
    while (block) {
      const auto block_sum = block->total_volume();
      if (block_sum > 0 && accumulated + block_sum >= target) {
        for (size_type i = 0; i < block->size(); ++i) {
          accumulated += (*block)[i].volume;
          if (accumulated >= target) {
            return {block, i};
          }
        }
      }
      accumulated += block_sum;
      block = block->next();
    }
    return {nullptr, 0};
  }

  void on_insert(BlockType* block, const value_type& value) {
    if (index_active_) {
      block_index_[value.id] = block;
    }
  }

  void on_remove(std::uint64_t id) {
    if (index_active_) {
      block_index_.erase(id);
    }
  }

  void activate_index_if_needed() {
    if (block_count_ >= 2 && !index_active_) {
      rebuild_index();
      index_active_ = true;
    }
  }

  void deactivate_index() {
    if (!index_active_) {
      return;
    }
    block_index_.clear();
    index_active_ = false;
  }

  void rebuild_index() {
    block_index_.clear();
    if (block_count_ < 2) {
      return;
    }
    for (BlockType* block = head_; block; block = block->next()) {
      for (size_type i = 0; i < block->size(); ++i) {
        block_index_[(*block)[i].id] = block;
      }
    }
  }

  void move_from(VolumeBreakdown&& other) {
    head_ = other.head_;
    tail_ = other.tail_;
    size_ = other.size_;
    block_count_ = other.block_count_;
    index_active_ = other.index_active_;
    block_index_ = std::move(other.block_index_);
    other.head_ = other.tail_ = nullptr;
    other.size_ = 0;
    other.block_count_ = 0;
    other.index_active_ = false;
    other.block_index_.clear();
  }

  BlockType* head_{nullptr};
  BlockType* tail_{nullptr};
  size_type size_{0};
  size_type block_count_{0};
  bool index_active_{false};
  absl::flat_hash_map<std::uint64_t, BlockType*> block_index_;
};
