#pragma once

#include "typestring/typestring.hpp"

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <numeric>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
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

        struct parse_handle_t
        {
            // parse_handle_t exists because std::function is 64 bytes
            // as opposed to 16 bytes
            expected<bool> operator()(std::string_view token)
            {
                return call(token, target);
            }

            void* target;
            expected<bool> (*call)(std::string_view, void*);
        };

        struct parse_visitor_t
        {
            parse_visitor_t() = default;
            parse_visitor_t(std::string_view token) : token{token} {}
            parse_visitor_t(const parse_visitor_t&) = default;

            auto operator()(auto* target) const
            {
                return detail::builtin_parse(token, target);
            }

            auto operator()(parse_handle_t handle) const
            {
                return handle(token);
            }

            std::string_view token;
        };

        enum class token_kind_t
        {
            literal,
            arg,
            flag,
        };

        template <std::convertible_to<std::string_view> String>
        inline auto semantic_tokenize(std::span<String> args)
        {
            auto ret = std::pair{std::vector<std::string_view>{},
                                 std::vector<token_kind_t>{}};
            auto& [tokens, kinds] = ret;

            for(size_t i = 0; i < args.size(); i++)
            {
                std::string_view token = args[i];
                if(!token.starts_with('-') || token.size() < 2)
                {
                    tokens.push_back(token);
                    kinds.push_back(token_kind_t::literal);
                    continue;
                }

                if(token == "--"sv)
                {
                    tokens.insert(tokens.end(), args.begin() + i + 1,
                                  args.end());
                    kinds.insert(kinds.end(), args.size() - i - 1,
                                 token_kind_t::literal);
                    return ret;
                }

                auto equal_pos = token.find('=');
                if(token[1] == '-')
                {
                    tokens.push_back(token.substr(2, equal_pos - 2));
                    kinds.push_back(token_kind_t::flag);
                }
                else
                {
                    auto end = std::min(equal_pos, token.size());
                    for(size_t j = 1; j < end; j++)
                    {
                        tokens.push_back(token.substr(j, 1));
                        kinds.push_back(token_kind_t::flag);
                    }
                }

                if(equal_pos != token.npos)
                {
                    tokens.push_back(token.substr(equal_pos + 1));
                    kinds.push_back(token_kind_t::arg);
                }
            }
            return ret;
        }
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

        auto operator()(std::string_view token)
        {
            return std::visit(detail::parse_visitor_t{token}, target);
        }

        using parseable_t =
            std::variant<bool*, char*, unsigned char*, signed char*, short*,
                         unsigned short*, int*, unsigned int*, long*,
                         unsigned long*, long long*, unsigned long long*,
                         float*, double*, long double*, std::string*,
                         std::string_view*, detail::parse_handle_t>;
        // TODO: differentiate user vs built-in function for flag_kind for user
        // extensions

        flag_kind_t kind() const
        {
            switch(target.index())
            {
            case 0: return boolean;
            case std::variant_size_v<parseable_t> - 1: return variable;
            default: return single;
            }
        }

        parseable_t target;
    };

    template <detail::single_parseable T>
    inline parse_arg_t make_parse_arg(T& target)
    {
        return parse_arg_t{.target = &target};
    }

    template <detail::parseable_container T>
    inline parse_arg_t make_parse_arg(T& target)
    {
        auto call = [](std::string_view token, void* target) {
            return detail::builtin_parse_container(token,
                                                   static_cast<T*>(target));
        };
        auto handle = detail::parse_handle_t{.target = &target, .call = call};
        return {.target = handle};
    }

    class parser_t
    {
      public:
        // Parses a span of arguments.
        // Typically, you pass std::span{argv + 1, argv + argc} from main.
        // Returns the remaining arguments that aren't part of flags.
        template <std::convertible_to<std::string_view> String>
        [[nodiscard]] expected<std::vector<std::string_view>>
        parse(std::span<String> args);

        // Adds a flag to the parser, with constexpr name and help.
        // target can be a basic type, std::string, std::string_view or a
        // container of them.
        // target must be kept alive throught the lifetime of parser_t.
        // Equivalent to vflag(name, help, make_parse_arg(target))
        void flag(flag_name_t name, help_str_t help,
                  builtin_parseable auto& target);

        // Same as flag except name and help can be runtime values.
        // parse_arg is constructed by calling make_parse_arg(target).
        // The string that name and help refers to must be kept alive thoughout
        // the lifetime of parser_t.
        void vflag(std::string_view name, std::string_view help,
                   parse_arg_t parse_arg);

        // Writes the options string to it.
        // The length of the string can be computed by options_len.
        template <std::output_iterator<char> It>
        auto options_to(It it) const -> It;

        // Returns the options string.
        [[nodiscard]] std::string options() const;

        // Computes the length of the options string.
        // help_newlines is the number of '\n' in help strings.
        [[nodiscard]] size_t options_len(int help_newlines = 0) const;

      private:
        struct flag_info_t
        {
            std::string_view name, help;
            parse_arg_t parse_arg;
        };
        // TODO: rewrite flag_info into four vectors:
        //  (name, help, parse_arg, index)
        // index is the original order, as opposed to the sorted order
        std::vector<flag_info_t> flag_info;

        void unguarded_vflag(std::string_view name, std::string_view help,
                             parse_arg_t parse_arg);

        static size_t dashed_len(std::string_view name);
        static size_t flag_len(const flag_info_t& x);
    };

    template <std::convertible_to<std::string_view> String>
    expected<std::vector<std::string_view>>
    parser_t::parse(std::span<String> args)
    {
        // TODO: currently err_unknown = false is not implemented correctly
        static constexpr bool err_unknown = true;

        using detail::token_kind_t;
        auto [tokens, kinds] = detail::semantic_tokenize(args);

        std::vector<std::string_view> remaining;
        parse_arg_t* parse_arg = nullptr;
        auto tb = tokens.begin();
        auto kb = kinds.begin(), ke = kinds.end();

        auto end_of_flag = [](auto& parse_arg, auto& tb) -> expected<void>
        {
            if(parse_arg->kind() == parse_arg_t::single)
            {
                auto prevtoken = tb[-1];
                auto dashes = prevtoken.size() > 1 ? "--"sv : "-"sv;
                return std::unexpected{
                    std::format("missing value after {}{}", dashes, prevtoken)};
            }
            else
            {
                auto result = (*parse_arg)({});
                if(!result)
                    return std::unexpected{std::move(result.error())};
                // postcondition: !result.value()
                parse_arg = nullptr;
            }
            return {};
        };

        for(; kb != ke; kb++)
        {
            switch(*kb)
            {
            case token_kind_t::literal:
            {
                if(!parse_arg)
                {
                    remaining.push_back(*tb);
                }
                else if(parse_arg->kind() == parse_arg_t::boolean)
                {
                    (void)(*parse_arg)({});
                    parse_arg = nullptr;
                    remaining.push_back(*tb);
                }
                else
                {
                    auto result = (*parse_arg)(*tb);
                    if(!result)
                        return std::unexpected{std::move(result.error())};
                    if(!result.value())
                        parse_arg = nullptr;
                }

                tb++;
                break;
            }
            case token_kind_t::arg:
            {
                // precondition: parse_arg != nullptr
                auto result = (*parse_arg)(*tb);
                if(!result)
                    return std::unexpected{std::move(result.error())};
                if(!result.value())
                    parse_arg = nullptr;

                tb++;
                break;
            }
            case token_kind_t::flag:
            {
                if(parse_arg)
                {
                    auto result = end_of_flag(parse_arg, tb);
                    if(!result)
                        return std::unexpected{result.error()};
                }

                auto token = *tb;
                auto it =
                    std::find_if(flag_info.begin(), flag_info.end(),
                                 [token](auto& x) { return x.name == token; });
                if(it == flag_info.end())
                {
                    if(err_unknown)
                    {
                        auto dashes = token.size() > 1 ? "--"sv : "-"sv;
                        return std::unexpected{
                            std::format("unknown flag {}{}", dashes, token)};
                    }

                    remaining.push_back(token);
                }
                else
                {
                    parse_arg = &it->parse_arg;
                }

                tb++;
                break;
            }
            }
        }

        if(parse_arg)
        {
            auto result = end_of_flag(parse_arg, tb);
            if(!result)
                return std::unexpected{result.error()};
        }

        return remaining;
    }

    inline void parser_t::flag(flag_name_t name, help_str_t help,
                               builtin_parseable auto& target)
    {
        unguarded_vflag(name.str, help.str, make_parse_arg(target));
    }

    inline void parser_t::vflag(std::string_view name, std::string_view help,
                                parse_arg_t parse_arg)
    {
        if(detail::invalid_name(name))
            throw std::runtime_error{std::format("invalid flag name {}", name)};
        unguarded_vflag(name, help, parse_arg);
    }

    template <std::output_iterator<char> It>
    auto parser_t::options_to(It it) const -> It
    {
        using namespace std::ranges;

        size_t longest = max(flag_info | views::transform(flag_len));
        std::string indent(longest + 6, ' ');

        for(auto& [name, help, _] : flag_info)
        {
            auto dashes = name.size() > 1 ? "--"sv : "-"sv;
            it = std::format_to(it, "{:>{}}{}{}  ", ' ',
                                longest - dashed_len(name) + 4, dashes, name);

            for(auto c : help)
            {
                *it++ = c;
                if(c == '\n')
                    it = std::copy(indent.begin(), indent.end(), it);
            }
            *it++ = '\n';
        }
        return it;
    }

    inline std::string parser_t::options() const
    {
        std::string buf;
        buf.reserve(options_len());

        options_to(std::back_inserter(buf));
        return buf;
    }

    inline size_t parser_t::options_len(int help_newlines) const
    {
        using namespace std::ranges;
        size_t longest = max(flag_info | views::transform(flag_len));

        // approximate due to newline in help requiring indentation
        auto approx_help_lens =
            flag_info | views::transform([](auto& x) { return x.help.size(); });
        size_t approx_sum = std::accumulate(approx_help_lens.begin(),
                                            approx_help_lens.end(), 0);

        // Padding is longest + 6, each entry in flag_info also requires an
        // extra newline.
        // Keep in sync with options_to format.
        return approx_sum + (longest + 6) * (flag_info.size() + help_newlines) +
               flag_info.size();
    }

    inline void parser_t::unguarded_vflag(std::string_view name,
                                          std::string_view help,
                                          parse_arg_t parse_arg)
    {
        // precondition: name.size() > 1
        if(name[1] == '-')
            name = name.substr(2);
        else
            name = name.substr(1);

        flag_info.push_back(
            {.name = name, .help = help, .parse_arg = parse_arg});
    }

    inline size_t parser_t::dashed_len(std::string_view name)
    {
        return name.size() + 1 + (name.size() > 1);
    }

    inline size_t parser_t::flag_len(const flag_info_t& x)
    {
        return dashed_len(x.name);
    };

} // namespace cozy
