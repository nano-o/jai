// -*-C++-*-

#pragma once

#include "err.h"

#include <charconv>
#include <concepts>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace options {

using std::size_t;

struct OptionError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template<std::constructible_from<std::string_view> T>
inline T
option_convert(std::string_view arg)
{
  return T(arg);
}

template<std::integral T>
T
option_convert(std::string_view arg)
{
  T val{};
  auto [ptr, ec] = std::from_chars(arg.begin(), arg.end(), val, 10);
  if (ptr != arg.end() || ptr == arg.begin())
    err<OptionError>(R"(invalid integer "{}")", arg);
  return val;
}

struct OptConverter {
  std::string_view arg;
  template<typename T> requires requires { option_convert<T>(""); }
  operator T() const
  {
    return option_convert<T>(arg);
  }
};

template<typename F> concept is_noarg_action = std::invocable<F>;
template<typename F>
concept is_arg_action = requires(F f) { f(OptConverter{}); };
template<typename F> concept is_action = is_noarg_action<F> || is_arg_action<F>;

struct Action {
  enum HasArg { kNoArg = 1, kArg = 2, kOptArg = 3 };

  virtual ~Action() {};
  virtual HasArg has_arg() const noexcept = 0;
  virtual void operator()() = 0;
  virtual void operator()(std::string_view) = 0;
};

template<is_action F> struct ActionImpl : Action {
  F f_;

  ActionImpl(F f) noexcept : f_(std::move(f)) {}
  HasArg has_arg() const noexcept override
  {
    return HasArg((is_noarg_action<F> ? kNoArg : 0) |
                  (is_arg_action<F> ? kArg : 0));
  }
  void operator()() override
  {
    if constexpr (is_noarg_action<F>)
      f_();
    else
      throw std::logic_error("option requires argument");
  }
  void operator()(std::string_view arg) override
  {
    if constexpr (is_arg_action<F>)
      f_(OptConverter{arg});
    else
      throw std::logic_error("option should not have argument");
  }
};
template<typename F> ActionImpl(F) -> ActionImpl<F>;

struct Option : std::string_view {
  template<std::size_t N> requires (N >= 3)
  consteval Option(const char (&str)[N]) : std::string_view(str, N - 1)
  {
    if (str[0] != '-' || (N == 3 && str[1] == '-'))
      throw R"(Option must be of form "-c" or "--string")";
    if (find('=') != npos)
      throw "Option name must not contain '='";
  }
};

struct Options {
  using enum Action::HasArg;
  std::map<std::string, std::shared_ptr<Action>, std::less<>> actions_;
  std::string help_;

  Options &add(std::initializer_list<Option> options, is_action auto f,
               std::string help = {}, std::string valname = {})
  {
    auto action = std::make_shared<ActionImpl<decltype(f)>>(std::move(f));
    if (valname.empty())
      valname = "VAL";
    std::string optstr;
    for (const auto &opt : options) {
      actions_[std::string(opt)] = action;
      if (help.empty())
        continue;
      if (!optstr.empty())
        optstr += ", ";
      optstr += opt;
      if (auto ha = action->has_arg(); ha != kNoArg) {
        if (ha == kOptArg)
          optstr += '[';
        else if (opt.size() == 2)
          optstr += ' ';
        if (opt.size() > 2)
          optstr += '=';
        optstr += valname;
        if (ha == kOptArg)
          optstr += ']';
      }
    }
    if (!help.empty()) {
      if (auto sz = optstr.size(); sz < 13)
        help_ += std::format("  {}{:<{}}{}\n", optstr, "", 14 - sz, help);
      else
        help_ += std::format("  {}\n    {}\n", optstr, help);
    }
    return *this;
  }

  Options &add(Option opt, is_action auto f, std::string help = {},
               std::string valname = {})
  {
    return add({opt}, std::move(f), std::move(help), std::move(valname));
  }

  Action &getopt(std::string_view opt)
  {
    if (auto it = actions_.find(opt); it != actions_.end())
      return *it->second;
    err<OptionError>("unknown option {}", opt);
  }

  template<std::convertible_to<std::string_view> S>
  std::span<S> parse_cli(std::span<S> args)
  {
    for (size_t i = 0; i < args.size(); ++i) {
      auto optarg = std::string_view(args[i]);
      if (optarg == "--")
        return args.subspan(i + 1);
      if (optarg.size() < 2 || optarg.front() != '-')
        return args.subspan(i);
      if (optarg[1] == '-') {
        std::string_view opt, arg;
        if (auto n = optarg.find('='); n == optarg.npos)
          opt = optarg;
        else {
          opt = optarg.substr(0, n);
          arg = optarg.substr(n);
        }
        auto &act = getopt(opt);
        auto ha = act.has_arg();
        if (!arg.empty()) {
          if (ha == kNoArg)
            err<OptionError>("option {} takes no argument", opt);
          act(arg.substr(1));
        }
        else if (ha != kArg)
          act();
        else if (i + 1 == args.size())
          err<OptionError>("option {} requires an argument", opt);
        else
          act(args[++i]);
      }
      else
        for (size_t j = 1; j < optarg.size(); j++) {
          auto &act = getopt(std::string({'-', optarg[j]}));
          auto ha = act.has_arg();
          if (ha == kNoArg) {
            act();
            continue;
          }
          if (j + 1 < optarg.size())
            act(optarg.substr(j + 1));
          else if (ha == kOptArg)
            act();
          else if (i + 1 == args.size())
            err<OptionError>("option -{} requires an argument", optarg[j]);
          else
            act(args[++i]);
          break;
        }
    }
    return {};
  }

  std::span<char *> parse_argv(int argc, char **argv)
  {
    return parse_cli(std::span{argv + 1, argv + argc});
  }
  void parse_file(std::string_view contents)
  {
    static constexpr std::string_view ws(" \t\r\n");
    for (auto lineview : contents | std::views::split('\n')) {
      auto line = std::string_view(lineview);
      auto b1 = line.find_first_not_of(ws);
      if (b1 == line.npos || line[b1] == '#')
        continue;
      auto e1 = std::min(line.find_first_of(ws, b1), line.size());
      auto opt = std::format("--{}", line.substr(b1, e1 - b1));
      if (auto b2 = line.find_first_not_of(ws, e1); b2 != line.npos) {
        auto e2 = line.find_last_not_of(ws);
        e2 = e2 == line.npos ? line.size() : e2 + 1;
        opt += std::format("={}", line.substr(b2, e2 - b2));
      }
      parse_cli(std::span{&opt, 1});
    }
  }
};

} // namespace options

using options::Options;
