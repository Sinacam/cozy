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
        concept single_parseable =
            is_in<T, std::string_view, std::string, bool> ||
            std::is_integral_v<T> || std::is_floating_point_v<T>;

        template <typename T>
        concept parseable_container = requires(T x) {
            typename T::value_type;
            requires !is_in<T, std::string, std::string_view>;
            requires single_parseable<typename T::value_type>;
            requires std::is_default_constructible_v<typename T::value_type>;
            {
                x.push_back(std::declval<typename T::value_type>())
            };
        };

        template <typename T>
            requires std::is_integral_v<T>
        expected<bool> builtin_parse(std::string_view s, T* target)
        {
            auto begin = s.data();
            auto end = s.data() + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, *target);
            if(ec != std::errc() || ptr != end)
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            return false;
        }

        template <typename T>
            requires std::is_floating_point_v<T>
        expected<bool> builtin_parse(std::string_view s, T* target)
        {
#if __cpp_lib_to_chars >= 201611L
            auto begin = s.data();
            auto end = s.data() + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, *target);
            if(ec != std::errc() || ptr != end)
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            return false;
#else
            T tmp;
            std::string str{s};
            char* str_end;

            if constexpr(std::is_same_v<double, T>)
                tmp = std::strtod(str.c_str(), &str_end);
            else if constexpr(std::is_same_v<float, T>)
                tmp = std::strtof(str.c_str(), &str_end);
            else
                tmp = std::strtold(str.c_str(), &str_end);

            if(str_end == str.c_str() || (str_end - str.c_str()) != s.size())
                return std::unexpected{std::format("cannot parse {} as {}", s,
                                                   typestring::name<T>)};
            *target = tmp;
            return false;
#endif
        }

        inline expected<bool> builtin_parse(std::string_view s, bool* target)
        {
            if(s.data() == nullptr || s == "true"sv)
                *target = true;
            else if(s == "false"sv)
                *target = false;
            else
                return std::unexpected{
                    std::format("cannot parse {} as bool", s)};
            return false;
        }

        template <typename T>
            requires is_in<T, std::string_view, std::string>
        expected<bool> builtin_parse(std::string_view s, T* target)
        {
            *target = s;
            return false;
        }

        template <parseable_container T>
        expected<bool> builtin_parse_container(std::string_view s, T* target)
        {
            if(s.data() == nullptr)
                return false;

            typename T::value_type v;
            auto result = builtin_parse(s, &v);
            if(!result)
                return result;
            target->push_back(std::move(v));

            return true;
        }

        struct parse_visitor_t
        {
            parse_visitor_t() = default;
            parse_visitor_t(std::string_view token) : token{token} {}
            parse_visitor_t(const parse_visitor_t&) = default;

            auto operator()(auto* target) const
            {
                return detail::builtin_parse(token, target);
            }

            auto operator()(
                std::function<expected<bool>(std::string_view)>& fn) const
            {
                return fn(token);
            }

            std::string_view token;
        };
    } // namespace detail

    struct flag_name_t
    {
        template <std::convertible_to<std::string_view> T>
        consteval flag_name_t(const T& name) : str{name}
        {
            if(detail::invalid_name(name))
                throw std::runtime_error("invalid flag name");
        }

        std::string_view str;
    };

    struct help_str_t
    {
        template <std::convertible_to<std::string_view> T>
        consteval help_str_t(const T& str) : str{str}
        {
        }

        std::string_view str;
    };

    template <typename T>
    concept builtin_parseable =
        detail::single_parseable<T> || detail::parseable_container<T>;

    struct parse_arg_t
    {
        enum flag_kind_t
        {
            single,
            boolean,
            variable,
        };

        using result_t = expected<bool>;
        auto operator()(std::string_view token)
        {
            return std::visit(detail::parse_visitor_t{token}, target);
        }

        using parseable_t = std::variant<
            bool*, char*, unsigned char*, signed char*, short*, unsigned short*,
            int*, unsigned int*, long*, unsigned long*, long long*,
            unsigned long long*, float*, double*, long double*, std::string*,
            std::string_view*, std::function<result_t(std::string_view)>>;

        parseable_t target;
        flag_kind_t flag_kind = single;
    };

    template <detail::single_parseable T>
    inline parse_arg_t make_parse_arg(T& target)
    {
        auto flag_kind = std::is_same_v<T, bool> ? parse_arg_t::boolean
                                                 : parse_arg_t::single;
        return parse_arg_t{.target = &target, .flag_kind = flag_kind};
    }

    template <detail::parseable_container T>
    inline parse_arg_t make_parse_arg(T& target)
    {
        return parse_arg_t{
            .target = [&target](std::string_view token)
            { return detail::builtin_parse_container(token, &target); },
            .flag_kind = parse_arg_t::variable};
    }

    class parser_t
    {

        struct flag_info_t
        {
            std::string_view name, help;
            parse_arg_t parse_arg;
        };

        static constexpr std::string_view plain_args_key = "";

      public:
        template <std::convertible_to<std::string_view> String>
        expected<std::vector<std::string_view>> parse(std::span<String> args,
                                                      bool err_unknown = true)
        {
            program_name = args[0];
            args = args.subspan(1);

            std::vector<std::string_view> remaining;
            parse_arg_t* parse_arg = nullptr;
            int i = 0;
            auto end_of_argument = [&parse_arg, &args, &i]
            {
                switch(parse_arg->flag_kind)
                {
                case parse_arg_t::single:
                    return std::unexpected{
                        std::format("missing value after {}",
                                    std::string_view{args[i - 1]})};
                case parse_arg_t::boolean: __builtin_unreachable();
                case parse_arg_t::variable:
                {
                    auto result = (*parse_arg)({});
                    if(!result)
                        return std::unexpected{result.error()};
                }
                default: __builtin_unreachable();
                }
            };

            for(; i < args.size(); i++)
            {
                std::string_view token = args[i];
                if(token.starts_with('-') && token.size() > 1)
                {
                    if(parse_arg)
                    {
                        end_of_argument();
                        parse_arg = nullptr;
                    }

                    if(token == "--")
                    {
                        i++;
                        break;
                    }

                    std::string_view equal_token;
                    auto pos = token.find('=');
                    if(pos != token.npos)
                    {
                        equal_token = token.substr(pos + 1);
                        token = token.substr(0, pos);
                    }

                    auto it = std::find_if(flag_info.begin(), flag_info.end(),
                                           [token](auto& x)
                                           { return x.name == token; });
                    if(it == flag_info.end() || it->name != token)
                    {
                        if(err_unknown)
                            return std::unexpected{
                                std::format("unknown flag {}", token)};

                        remaining.push_back(token);
                        continue;
                    }
                    parse_arg = &it->parse_arg;

                    if(equal_token.data() == nullptr)
                    {
                        if(parse_arg->flag_kind == parse_arg_t::boolean)
                        {
                            auto result = (*parse_arg)({});
                            if(!result)
                                return std::unexpected{result.error()};
                            parse_arg = nullptr;
                        }
                        continue;
                    }

                    auto result = (*parse_arg)(equal_token);
                    if(!result)
                        return std::unexpected{result.error()};
                    if(!result.value())
                        parse_arg = nullptr;
                }
                else if(parse_arg)
                {
                    auto result = (*parse_arg)(token);
                    if(!result)
                        return std::unexpected{result.error()};
                    if(!result.value())
                        parse_arg = nullptr;
                }
                else
                {
                    remaining.push_back(token);
                }
            }

            if(parse_arg)
                end_of_argument();

            for(; i < args.size(); i++)
                remaining.push_back(args[i]);

            return remaining;
        }

        void flag(flag_name_t name, help_str_t help,
                  builtin_parseable auto& target)
        {
            unguarded_vflag(name.str, help.str, make_parse_arg(target));
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

            auto name_len = [](auto& x) { return x.name.size(); };
            int longest = max(flag_info | views::transform(name_len));
            std::string indent(' ', longest + 8);

            for(auto& [name, help, _] : flag_info)
            {
                os << std::format("    {:>{}}  ", name, longest);
                for(auto c : help)
                {
                    os << c;
                    if(c == '\n')
                        os << indent;
                }
                os << '\n';
            }
        }

        std::string usage() const
        {
            std::stringstream ss;
            usage_to(ss);
            return ss.str();
        }

      private:
        std::vector<flag_info_t> flag_info;
        std::string_view program_name;

        void unguarded_vflag(std::string_view name, std::string_view help,
                             parse_arg_t parse_arg)
        {
            flag_info.push_back(
                {.name = name, .help = help, .parse_arg = parse_arg});
        }
    };
} // namespace cozy
