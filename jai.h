// -*-C++-*-

#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include <set>

#include <unistd.h>

template<typename T> concept has_no_cv = std::same_as<T, std::remove_cv_t<T>>;

// Format error message and throw an exception that captures errno
template<typename... Args>
[[noreturn]] inline void
syserr(std::format_string<Args...> fmt, Args &&...args)
{
  throw std::system_error(
      errno, std::system_category(),
      std::vformat(fmt.get(), std::make_format_args(args...)));
}

// Format error message and throw exception
template<typename E = std::runtime_error, typename... Args>
[[noreturn]] inline void
err(std::format_string<Args...> fmt, Args &&...args)
{
  throw E(std::vformat(fmt.get(), std::make_format_args(args...)));
}

template<typename T, typename... Ts>
concept is_one_of = (std::same_as<T, Ts> || ...);

// Note that Destroy generally should not throw, whether or not it is
// declared noexcept.  Only an explicit call to reset() will allow
// exceptions to propagate.
template<typename T, auto Empty, auto Destroy> struct RaiiHelper {
  T t_ = Empty;

  RaiiHelper() noexcept = default;
  RaiiHelper(T t) noexcept : t_(std::move(t)) {}
  RaiiHelper(RaiiHelper &&other) noexcept : t_(other.release()) {}
  ~RaiiHelper() { reset(); }

  template<is_one_of<T, decltype(Empty)> Arg>
  RaiiHelper &operator=(Arg &&arg) noexcept
  {
    reset(std::forward<Arg>(arg));
    return *this;
  }
  RaiiHelper &operator=(RaiiHelper &&other) noexcept
  {
    return *this = other.release();
  }

  explicit operator bool() const noexcept { return t_ != Empty; }
  const T &operator*() const noexcept { return t_; }

  T release() noexcept { return std::exchange(t_, Empty); }

  template<is_one_of<T, decltype(Empty)> Arg>
  void reset(Arg &&arg) noexcept(noexcept(Destroy(std::move(t_))))
  {
    if (auto destroy_me = std::exchange(t_, std::forward<Arg>(arg));
        destroy_me != Empty)
      Destroy(std::move(destroy_me));
  }
  void reset() noexcept(noexcept(reset(Empty))) { reset(Empty); }
};

// Self-closing file descriptor
using Fd = RaiiHelper<int, -1, ::close>;

namespace detail {
struct NullaryInvoker {
  template<typename F> static decltype(auto) operator()(F &&f)
  {
    return std::forward<F>(f)();
  }
};
} // namespace detail
// Deferred cleanup action
using Defer = RaiiHelper<std::move_only_function<void()>, nullptr,
                         detail::NullaryInvoker{}>;

using std::filesystem::path;


// Compare paths component by component so subtrees are contiguous
struct PathLess {
  static bool operator()(const path &a, const path &b)
  {
    return std::ranges::lexicographical_compare(a, b);
  }
};

using PathSet = std::multiset<path, PathLess>;

// Return a range for a subtree rooted at root.  root itself will be
// returned only if it does not contain a trailing slash.
inline auto
subtree(const PathSet &s, const path &root)
{
  // First possible pathname not under root is the (illegal) pathname
  // in which root's final component has a '\0' byte appended.
  auto end = root.string();
  if (!end.empty() && end.back() == '/')
    end.back() = '\0';
  else
    end += '\0';
  return std::ranges::subrange(s.lower_bound(root), s.lower_bound(path(end)));
}

// Return a subtree in reverse order (suitable for unmounting).
inline auto
subtree_rev(const PathSet &s, const path &root)
{
  return subtree(s, root) | std::views::reverse;
}
