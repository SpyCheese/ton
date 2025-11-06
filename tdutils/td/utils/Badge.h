#pragma once

namespace td {

template <typename T>
class Badge {
 public:
  constexpr Badge(Badge&&) = default;
  constexpr Badge& operator=(Badge&&) = default;

  Badge(Badge const&) = delete;
  Badge& operator=(Badge const&) = delete;

 private:
  friend T;

  constexpr Badge() = default;
};

}  // namespace td
