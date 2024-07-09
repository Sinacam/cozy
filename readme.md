# Cozy
Cozy is a lightweight command line flag library for C++23. It is a single header `cozy.hpp`.

## Why C++23?
Because it is written for myself and I have up-to-date compilers.

## Modules?
While I'd love to provide modules, gcc and clang still doesn't have full support as of writing, so no.

## Usage
```c++
#include "cozy.hpp"

#include <iostream>
#include <vector>
#include <span>
#include <string>

int main(int argc, char** argv)
{
    int n = 42;
    bool h = false;
    std::vector<int> v;
    std::string str;

    cozy::parser_t parser;
    parser.flag("-n", "the second argument is the help string", n);
    parser.flag("--str", "multi-character flags starts with --", str);
    parser.flag("-v", "containers take an arbitrary number of arguments", v);
    parser.flag("-h", "bool flags can be set by '-h=true' or '-h' but not '-h true'", h);

    auto remaining = parser.parse(std::span{argv + 1, argv + argc});
    if(!remaining)
    {
        std::cerr << remaining.error();
        return 1;
    }
    if(h)
    {
        std::cout << "usage of program:\n" << parser.options();
        return 0;
    }
}
```

This program can be invoked as
```bash
./program -n 420 --str "to be or not to be" -v 0 1 2 3
./program -h
```

For arguments starting with `-`, use `=` without spaces
```bash
./program --str=- -n=-420
```

Arguments that aren't part of flags are stored in `remaining`
```bash
./program remaining0 -n 420 remaining1
```

Use `--` to delimit end of flags
```bash
./program -n 420 -- -n not a flag anymore
```
