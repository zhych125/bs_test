#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

template <typename T, std::size_t Capacity = 64>
class Block {
  static_assert(Capacity > 0, "Block capacity must be positive");
  static_assert(std::is_trivially_copyable_v<T>,
                "Block requires trivially copyable value types for memmove-based shifts");
  static_assert(std::is_trivially_destructible_v<T>,
                "Block requires trivially destructible value types");

 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = T*;
  using const_iterator = const T*;
  Block* next() { return next_; }
  Block* prev() { return prev_; }
  const Block* next() const { return next_; }
  const Block* prev() const { return prev_; }
  void set_next(Block* next) { next_ = next; }
  void set_prev(Block* prev) { prev_ = prev; }

  Block() = default;

  constexpr size_type capacity() const { return Capacity; }
  size_type size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == Capacity; }

  iterator begin() { return data(); }
  iterator end() { return data() + size_; }
  const_iterator begin() const { return data(); }
  const_iterator end() const { return data() + size_; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  T& operator[](size_type index) { return *slot(index); }
  const T& operator[](size_type index) const { return *slot(index); }

  T& front() { return *slot(0); }
  const T& front() const { return *slot(0); }
  T& back() { return *slot(size_ - 1); }
  const T& back() const { return *slot(size_ - 1); }

  template <typename... Args>
  T& emplace_back(Args&&... args) {
    assert(!full());
    T* dest = slot(size_);
    new (dest) T(std::forward<Args>(args)...);
    ++size_;
    total_volume_ += dest->volume;
    return *dest;
  }

  void push_back(const T& value) { emplace_back(value); }
  void push_back(T&& value) { emplace_back(std::move(value)); }

  template <typename... Args>
  T& emplace_front(Args&&... args) {
    assert(!full());
    if (size_ > 0) {
      std::memmove(slot(1), slot(0), size_ * sizeof(T));
    }
    new (slot(0)) T(std::forward<Args>(args)...);
    ++size_;
    total_volume_ += front().volume;
    return front();
  }

  void push_front(const T& value) { emplace_front(value); }
  void push_front(T&& value) { emplace_front(std::move(value)); }

  void pop_back() {
    assert(!empty());
    const auto removed_volume = back().volume;
    --size_;
    total_volume_ -= removed_volume;
  }

  void pop_front() {
    assert(!empty());
    const auto removed_volume = front().volume;
    const size_type tail_count = size_ - 1;
    if (tail_count > 0) {
      std::memmove(slot(0), slot(1), tail_count * sizeof(T));
    }
    total_volume_ -= removed_volume;
    --size_;
  }

  void clear() {
    size_ = 0;
    total_volume_ = 0;
  }

  void erase(size_type index) {
    assert(index < size_);
    const auto removed_volume = (*this)[index].volume;
    const size_type tail_count = size_ - index - 1;
    if (tail_count > 0) {
      std::memmove(slot(index), slot(index + 1), tail_count * sizeof(T));
    }
    total_volume_ -= removed_volume;
    --size_;
  }

  template <typename Predicate>
  T* find_if(Predicate&& pred) {
    for (size_type i = 0; i < size_; ++i) {
      if (pred(*slot(i))) {
        return slot(i);
      }
    }
    return nullptr;
  }

  template <typename Predicate>
  const T* find_if(Predicate&& pred) const {
    for (size_type i = 0; i < size_; ++i) {
      if (pred(*slot(i))) {
        return slot(i);
      }
    }
    return nullptr;
  }

  std::int64_t total_volume() const { return total_volume_; }

 private:
  T* data() { return reinterpret_cast<T*>(storage_.data()); }
  const T* data() const { return reinterpret_cast<const T*>(storage_.data()); }

  T* slot(size_type index) { return data() + index; }
  const T* slot(size_type index) const { return data() + index; }

  Block* prev_{nullptr};
  Block* next_{nullptr};
  size_type size_{0};
  std::int64_t total_volume_{0};
  std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, Capacity> storage_{};
};
