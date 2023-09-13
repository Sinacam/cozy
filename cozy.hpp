#pragma once

#include "typestring/typestring.hpp"

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace cozy
{
    using namespace std::literals;

    template <typename T>
    using expected = std::expected<T, std::string>;

    struct parse_arg_t
    {
        using result_t = expected<bool>;
        auto operator()(std::string_view s) { return fn(s); }

        std::function<result_t(std::string_view)> fn;
        int allow_empty = false;
    };

    namespace detail
    {
        inline constexpr bool invalid_name(std::string_view name)
        {
            return !name.starts_with('-') ||
                   (name.size() > 2 && name[1] != '-') || name == "--" ||
                   name == "-" || name.find_first_of("= \t\n") != name.npos;
        }

        template <typename T, typename... Ts>
        inline constexpr bool is_in = (std::is_same_v<T, Ts> || ...);

        // disallowing cv qualifiers is deliberate
        template <typename T>
        inline constexpr bool is_single_parseable_v =
            is_in<T, std::string_view, std::string, bool> ||
            std::is_integral_v<T> || std::is_floating_point_v<T>;

        template <typename T>
        inline constexpr bool is_parseable_container_v = requires(T x) {
            typename T::value_type;
            requires is_single_parseable_v<typename T::value_type>;
            requires std::is_default_constructible_v<typename T::value_type>;
            {
                x.push_back(std::declval<typename T::value_type>())
            };
        };

        template <typename T>
            requires std::is_integral_v<T>
        expected<bool> builtin_parse(std::string_view s, T& x)
        {
            auto begin = s.data();
            auto end = s.data() + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, x);
            if(ec != std::errc() || ptr != end)
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            return false;
        }

        template <typename T>
            requires std::is_floating_point_v<T>
        expected<bool> builtin_parse(std::string_view s, T& x)
        {
#if __cpp_lib_to_chars >= 201611L
            auto begin = s.data();
            auto end = s.data() + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, x);
            if(ec != std::errc() || ptr != end)
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            return false;
#else
            try
            {
                if constexpr(std::is_same_v<double, T>)
                    x = stod(std::string(s));
                else if constexpr(std::is_same_v<float, T>)
                    x = stof(std::string(s));
                else
                    x = stold(std::string(s));
                return false;
            }
            catch(...)
            {
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            }
#endif
        }

        inline expected<bool> builtin_parse(std::string_view s, bool& x)
        {
            if(s == ""sv || s == "true"sv)
                x = true;
            else if(s == "false"sv)
                x = false;
            else
                return std::unexpected{
                    std::format("cannot parse {} as bool", s)};
            return false;
        }

        template <typename T>
            requires is_in<T, std::string_view, std::string>
        expected<bool> builtin_parse(std::string_view s, T& x)
        {
            x = s;
            return false;
        }

        template <typename T>
            requires is_parseable_container_v<T>
        expected<bool> builtin_parse(std::string_view s, T& x)
        {
            typename T::value_type v;
            auto result = builtin_parse(s, v);
            if(!result)
                return result;
            x.push_back(std::move(v));
            return true;
        }

    } // namespace detail

    struct flag_name_t
    {
        template <size_t N>
        consteval flag_name_t(const char (&name)[N])
            : flag_name_t{std::string_view{name}}
        {
        }

        consteval flag_name_t(std::string_view name) : str{name}
        {
            if(detail::invalid_name(name))
                throw std::runtime_error("invalid flag name");
        }

        std::string_view str;
    };

    template <typename T>
    concept builtin_parseable =
        detail::is_single_parseable_v<T> || detail::is_parseable_container_v<T>;

    template <builtin_parseable T>
    inline parse_arg_t make_parse_arg(T& target)
    {
        auto fn = [&target](std::string_view s)
        { return detail::builtin_parse(s, target); };
        return parse_arg_t{.fn = fn, .allow_empty = std::is_same_v<T, bool>};
    }

    class parser_t
    {
        static constexpr std::string_view plain_args_key = "";

      public:
        template <std::convertible_to<std::string_view> String>
        expected<std::vector<std::string_view>> parse(std::span<String> args)
        {
            program_name = args[0];
            args = args.subspan(1);

            std::vector<std::string_view> remaining;
            parse_arg_t* parse_arg = nullptr;

            int i = 0;
            for(; i < args.size(); i++)
            {
                std::string_view arg = args[i];
                if(arg.starts_with('-'))
                {
                    if(parse_arg)
                    {
                        if(!parse_arg->allow_empty)
                            return std::unexpected{
                                std::format("missing value after {}",
                                            std::string_view{args[i - 1]})};
                        auto result = (*parse_arg)(""sv);
                        if(!result)
                            return std::unexpected{result.error()};
                        parse_arg = nullptr;
                    }

                    if(arg == "--")
                    {
                        i++;
                        break;
                    }

                    auto it = parse_args.find(arg);
                    if(it == parse_args.end())
                    {
                        remaining.push_back(arg);
                        continue;
                    }
                    parse_arg = &it->second;
                }
                else if(parse_arg)
                {
                    auto result = (*parse_arg)(arg);
                    if(!result)
                        return std::unexpected{result.error()};
                    if(!result.value())
                        parse_arg = nullptr;
                }
                else
                {
                    remaining.push_back(arg);
                }
            }

            if(parse_arg)
            {
                if(!parse_arg->allow_empty)
                    return std::unexpected{
                        std::format("missing value after {}",
                                    std::string_view{args.back()})};
                auto result = (*parse_arg)(""sv);
                if(!result)
                    return std::unexpected{result.error()};
            }

            for(; i < args.size(); i++)
                remaining.push_back(args[i]);

            return remaining;
        }

        void flag(flag_name_t name, std::string_view help,
                  builtin_parseable auto& target)
        {
            unguarded_vflag(name.str, help, make_parse_arg(target));
        }

        void vflag(std::string_view name, std::string_view help,
                   parse_arg_t parse_arg)
        {
            if(detail::invalid_name(name))
                throw std::runtime_error{
                    std::format("invalid flag name {}", name)};
            unguarded_vflag(name, help, parse_arg);
        }

        void usage_to(std::ostream& os) const
        {
            using namespace std::ranges;

            if(program_name.size() > 0)
                os << std::format("Usage of {}:\n", program_name);
            else
                os << std::format("Usage:\n");

            int longest =
                max(help_strs |
                    views::transform([](auto& hs) { return hs.first.size(); }));
            for(auto [name, help] : help_strs)
                os << std::format("    {:{}}{}\n", name, longest + 4, help);
        }

        std::string usage() const
        {
            std::stringstream ss;
            usage_to(ss);
            return ss.str();
        }

      private:
        std::unordered_map<std::string_view, parse_arg_t> parse_args;
        std::vector<std::pair<std::string_view, std::string_view>> help_strs;
        std::string_view program_name;

        void unguarded_vflag(std::string_view name, std::string_view help,
                             parse_arg_t parse_arg)
        {
            if(parse_args.contains(name))
            {
                throw std::runtime_error{
                    std::format("flag {} already exists", name)};
            }

            parse_args[name] = parse_arg;
            help_strs.push_back({name, help});
        }
    };

} // namespace cozy
