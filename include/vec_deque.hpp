#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

// A minimal Rust-like VecDeque implemented as a power-of-two ring buffer.
// Supports push/pop at both ends, random access, and random-access iterators.
template <typename T, typename Allocator = std::allocator<T>>
class VecDeque {
 public:
  using value_type = T;
  using allocator_type = Allocator;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;

  VecDeque() = default;
  explicit VecDeque(const Allocator& alloc) : alloc_(alloc) {}

  VecDeque(const VecDeque& other) { copy_from(other); }
  VecDeque(VecDeque&& other) noexcept { move_from(std::move(other)); }

  VecDeque& operator=(const VecDeque& other) {
    if (this != &other) {
      clear();
      copy_from(other);
    }
    return *this;
  }

  VecDeque& operator=(VecDeque&& other) noexcept {
    if (this != &other) {
      destroy_storage();
      move_from(std::move(other));
    }
    return *this;
  }

  ~VecDeque() { destroy_storage(); }

  allocator_type get_allocator() const { return alloc_; }

  bool empty() const { return size_ == 0; }
  size_type size() const { return size_; }
  size_type capacity() const { return capacity_; }

  reference front() { return (*this)[0]; }
  const_reference front() const { return (*this)[0]; }

  reference back() { return (*this)[size_ - 1]; }
  const_reference back() const { return (*this)[size_ - 1]; }

  reference operator[](size_type index) { return data_[physical_index(index)]; }
  const_reference operator[](size_type index) const { return data_[physical_index(index)]; }

  void clear() {
    for (size_type i = 0; i < size_; ++i) {
      destroy_at(physical_index(i));
    }
    head_ = 0;
    size_ = 0;
  }

  void reserve(size_type new_cap) {
    if (new_cap <= capacity_) {
      return;
    }
    grow_to(next_power_of_two(new_cap));
  }

  void push_back(const T& value) { emplace_back(value); }
  void push_back(T&& value) { emplace_back(std::move(value)); }
  template <typename... Args>
  reference emplace_back(Args&&... args) {
    ensure_capacity(size_ + 1);
    const size_type slot = physical_index(size_);
    construct_at(slot, std::forward<Args>(args)...);
    ++size_;
    return data_[slot];
  }

  void push_front(const T& value) { emplace_front(value); }
  void push_front(T&& value) { emplace_front(std::move(value)); }
  template <typename... Args>
  reference emplace_front(Args&&... args) {
    ensure_capacity(size_ + 1);
    head_ = dec_index(head_);
    construct_at(head_, std::forward<Args>(args)...);
    ++size_;
    return data_[head_];
  }

  void pop_back() {
    if (empty()) {
      return;
    }
    const size_type idx = physical_index(size_ - 1);
    destroy_at(idx);
    --size_;
  }

  void pop_front() {
    if (empty()) {
      return;
    }
    destroy_at(head_);
    head_ = inc_index(head_);
    --size_;
  }

  template <bool IsConst>
  class iterator_base {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using reference = std::conditional_t<IsConst, const T&, T&>;
    using pointer = std::conditional_t<IsConst, const T*, T*>;

    iterator_base() = default;

    reference operator*() const { return deque_->operator[](index_); }
    pointer operator->() const { return &deque_->operator[](index_); }

    iterator_base& operator++() {
      ++index_;
      return *this;
    }
    iterator_base operator++(int) {
      iterator_base tmp = *this;
      ++(*this);
      return tmp;
    }
    iterator_base& operator--() {
      --index_;
      return *this;
    }
    iterator_base operator--(int) {
      iterator_base tmp = *this;
      --(*this);
      return tmp;
    }
    iterator_base& operator+=(difference_type n) {
      index_ += n;
      return *this;
    }
    iterator_base& operator-=(difference_type n) {
      index_ -= n;
      return *this;
    }
    iterator_base operator+(difference_type n) const {
      iterator_base tmp = *this;
      tmp += n;
      return tmp;
    }
    iterator_base operator-(difference_type n) const {
      iterator_base tmp = *this;
      tmp -= n;
      return tmp;
    }
    difference_type operator-(const iterator_base& other) const { return index_ - other.index_; }
    reference operator[](difference_type n) const { return *(*this + n); }

    bool operator==(const iterator_base& other) const {
      return deque_ == other.deque_ && index_ == other.index_;
    }
    bool operator!=(const iterator_base& other) const { return !(*this == other); }
    bool operator<(const iterator_base& other) const { return index_ < other.index_; }
    bool operator>(const iterator_base& other) const { return other < *this; }
    bool operator<=(const iterator_base& other) const { return !(other < *this); }
    bool operator>=(const iterator_base& other) const { return !(*this < other); }

   private:
    friend class VecDeque;
    using DequePtr = std::conditional_t<IsConst, const VecDeque*, VecDeque*>;

    iterator_base(DequePtr deque, size_type idx) : deque_(deque), index_(idx) {}

    DequePtr deque_{nullptr};
    size_type index_{0};
  };

  using iterator = iterator_base<false>;
  using const_iterator = iterator_base<true>;

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, size_); }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, size_); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  iterator erase(iterator pos) {
    if (pos == end()) {
      return pos;
    }
    size_type idx = pos - begin();
    if (idx < size_ / 2) {
      for (size_type i = idx; i > 0; --i) {
        (*this)[i] = std::move((*this)[i - 1]);
      }
      pop_front();
      return iterator(this, idx);
    } else {
      for (size_type i = idx; i + 1 < size_; ++i) {
        (*this)[i] = std::move((*this)[i + 1]);
      }
      pop_back();
      return iterator(this, idx);
    }
  }

 private:
  using AllocTraits = std::allocator_traits<Allocator>;

  T* data_{nullptr};
  Allocator alloc_{};
  size_type capacity_{0};
  size_type head_{0};
  size_type size_{0};

  void destroy_storage() {
    clear();
    if (data_) {
      AllocTraits::deallocate(alloc_, data_, capacity_);
      data_ = nullptr;
    }
    capacity_ = 0;
    head_ = 0;
  }

  static size_type next_power_of_two(size_type n) {
    if (n <= 1) {
      return 1;
    }
    --n;
    for (size_type i = 1; i < sizeof(size_type) * 8; i <<= 1) {
      n |= n >> i;
    }
    return n + 1;
  }

  void ensure_capacity(size_type desired) {
    if (desired <= capacity_) {
      return;
    }
    size_type new_cap = next_power_of_two(std::max<size_type>(desired, 1));
    grow_to(new_cap);
  }

  void grow_to(size_type new_cap) {
    T* new_data = AllocTraits::allocate(alloc_, new_cap);
    for (size_type i = 0; i < size_; ++i) {
      AllocTraits::construct(alloc_, new_data + i, std::move((*this)[i]));
      destroy_at(physical_index(i));
    }
    if (data_) {
      AllocTraits::deallocate(alloc_, data_, capacity_);
    }
    data_ = new_data;
    capacity_ = new_cap;
    head_ = 0;
  }

  template <typename... Args>
  void construct_at(size_type idx, Args&&... args) {
    AllocTraits::construct(alloc_, data_ + idx, std::forward<Args>(args)...);
  }

  void destroy_at(size_type idx) {
    AllocTraits::destroy(alloc_, data_ + idx);
  }

  size_type physical_index(size_type logical_index) const {
    if (capacity_ == 0) {
      return 0;
    }
    const size_type mask = capacity_ - 1;
    return (head_ + logical_index) & mask;
  }

  size_type inc_index(size_type idx) const { return (idx + 1) & (capacity_ - 1); }
  size_type dec_index(size_type idx) const { return (idx - 1) & (capacity_ - 1); }

  void copy_from(const VecDeque& other) {
    if (other.size_ == 0) {
      return;
    }
    capacity_ = next_power_of_two(other.size_);
    head_ = 0;
    size_ = other.size_;
    alloc_ = AllocTraits::select_on_container_copy_construction(other.alloc_);
    data_ = AllocTraits::allocate(alloc_, capacity_);
    for (size_type i = 0; i < size_; ++i) {
      AllocTraits::construct(alloc_, data_ + i, other[i]);
    }
  }

  void move_from(VecDeque&& other) {
    data_ = other.data_;
    alloc_ = std::move(other.alloc_);
    capacity_ = other.capacity_;
    head_ = other.head_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.capacity_ = 0;
    other.head_ = 0;
    other.size_ = 0;
  }
};
