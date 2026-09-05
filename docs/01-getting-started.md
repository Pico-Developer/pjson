# Chapter 01 — Getting started

In this chapter you will compile and run your very first pjson program. By the
end you will have printed a JSON value to the screen.

## What you need

- A C++ compiler that supports **C++11** or newer (g++, clang, or MSVC).
- pjson's canonical public header, `pjsonlib/include/pjson.h`.
- pjson's DOM and serialization sources, `pjsonlib/src/pjson.cpp` and
  `pjsonlib/src/pjson_serialize.cpp`, plus the vendored Ryu conversion source.

That's it. pjson has **no third-party dependencies**.

## The simplest program

This is [`examples/src/01_hello_world.cpp`](../examples/src/01_hello_world.cpp):

```cpp
#include "pjson.h"       // 1. bring in the library
#include <cstdint>
#include <iostream>

using namespace ByteDance; // 2. pjson lives in the ByteDance namespace

int main() {
    // 3. A fresh pjson is 'null'. Assigning to a key makes it an object.
    pjson greeting;
    greeting["message"] = "Hello, World!";
    greeting["year"] = int64_t(2025);

    // 4. Serialize to compact and pretty text.
    pjson::SerializeOptions compact;
    pjson::SerializeOptions pretty = pjson::SerializeOptions::prettyPrinted();
    std::cout << greeting.toString(compact) << "\n";
    std::cout << greeting.toString(pretty) << "\n";
    return 0;
}
```

Line by line:

1. `#include "pjson.h"` gives you the `pjson` class.
2. `using namespace ByteDance;` lets you write `pjson` instead of
   `ByteDance::pjson`. (You can skip this and qualify the name if you prefer.)
3. A default-constructed `pjson` holds `null`. The moment you assign to a
   string key with `greeting["message"] = ...`, pjson turns it into an
   **object**. (We cover this "auto-vivification" in Chapter 02.)
4. `toString(options)` returns the JSON as a `std::string`. Default options are
   compact; `SerializeOptions::prettyPrinted()` selects indented output. Both
   presets limit output to 64 MiB unless `maxOutputBytes` is changed; zero
   explicitly requests unlimited output.

## Compiling it

The fastest way, compiling the library source directly alongside your program:

```sh
c++ -std=c++11 -I pjsonlib/include \
    -I pjsonlib/src/third_party/ryu \
    pjsonlib/src/pjson.cpp pjsonlib/src/pjson_serialize.cpp \
    pjsonlib/src/third_party/ryu/ryu/d2s.c \
    examples/src/01_hello_world.cpp \
    -o hello
./hello
```

- `-std=c++11` selects the C++ standard.
- `-I pjsonlib/include` tells the compiler where to find `pjson.h`.
- We list the DOM, serializer, and Ryu sources used by this example. The CMake
  target shown in Chapter 08 is preferable once parsing or other components are
  needed because it maintains the complete source list.

Expected output:

```json
{"message":"Hello, World!","year":2025}
{
  "message": "Hello, World!",
  "year": 2025
}
```

Notice the compact form is one line, and the pretty form is indented. Also note
the keys came out in the order `message`, then `year` — which happens to be
alphabetical. (Recall from Chapter 00 that pjson sorts object keys for output.)

```mermaid
flowchart LR
    src["your .cpp + DOM + serializer + Ryu"] -->|"c++ -std=c++11 -I include"| exe["executable"]
    exe -->|run| out["JSON printed"]
```

## If you'd rather use the build script

From the repository root you can build everything (library + tests + examples)
with the bundled script — covered fully in
[Chapter 08](08-building-and-installing.md):

```sh
./build.sh
```

## Troubleshooting

- **`fatal error: 'pjson.h' file not found`** — you forgot `-I pjsonlib/include`
  (or the path to wherever you put the header).
- **`undefined reference to ByteDance::pjson::...`** — a required library
  translation unit was omitted; prefer linking the `pjson::pjson` CMake target.
- **Lots of syntax errors** — your compiler may be defaulting to an old
  standard; add `-std=c++11` (or newer).

## What you learned

- pjson has no dependencies that applications must install separately; its Ryu
  number-conversion dependency is vendored and built by the CMake target.
- A new `pjson` is `null`; assigning to a key makes it an object.
- `SerializeOptions` selects compact or pretty JSON serialization.
- Direct compilation must include the component sources used by the program;
  the CMake target supplies the complete list automatically.

Next: [Chapter 02 — Creating JSON](02-creating-json.md), where you build richer
objects and arrays.
