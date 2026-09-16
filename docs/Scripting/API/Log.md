# fjell::Log

Logs messages to the editor console.

| | |
|---|---|
| Header | `fjell/log_api.hpp` |
| Source  | `src/scripting/log_api.cpp` |

## Syntax

```cpp
namespace fjell {
struct Log {
    static void info (const char* msg);
    static void warn (const char* msg);
    static void error(const char* msg);

    static void info_f (const char* fmt, ...);
    static void warn_f (const char* fmt, ...);
    static void error_f(const char* fmt, ...);
};
}
```

## Remarks

The three severities map to spdlog levels and show up in the editor's console panel with distinct colors. The `_f` variants are `printf`-style with a 256-byte internal buffer — longer messages are truncated; format yourself and pass to the base call if you need more.

Format strings are checked against their arguments at compile time, so a mismatched specifier is a warning rather than a runtime garbage read. Formatting happens on the caller's side and only the finished string crosses the engine boundary.

Safe to call from any thread that the engine has handed control to (main, script tick, timer callbacks). Not safe from arbitrary worker threads.

## Functions

| Name | Description |
|---|---|
| [`info`](#loginfo) | Log an informational message |
| [`warn`](#logwarn) | Log a warning |
| [`error`](#logerror) | Log an error |
| [`info_f`](#loginfo_f) | Printf-style info log |
| [`warn_f`](#logwarn_f) | Printf-style warning log |
| [`error_f`](#logerror_f) | Printf-style error log |

---

## Log::info

### Syntax
```cpp
static void info(const char* msg);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `msg` | `const char*` | Null-terminated message. Must not be null. |

### Remarks
Routed to spdlog at `info` level. Shows in the editor console with default styling.

---

## Log::warn

### Syntax
```cpp
static void warn(const char* msg);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `msg` | `const char*` | Null-terminated message. Must not be null. |

### Remarks
Shown in the editor console at warning severity. Use for recoverable issues — null asset refs in non-fatal paths, fallbacks engaged, deprecated usage.

---

## Log::error

### Syntax
```cpp
static void error(const char* msg);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `msg` | `const char*` | Null-terminated message. Must not be null. |

### Remarks
Shown in the editor console at error severity. Use for failures that the script can't transparently recover from. Does not throw or abort.

---

## Log::info_f

### Syntax
```cpp
static void info_f(const char* fmt, ...);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `fmt` | `const char*` | Printf-style format string. |
| `...` | | Format arguments. Must be POD — pass `s.c_str()`, never a `std::string`. |

### Remarks
Formats into a 256-byte stack buffer with `std::vsnprintf`, then forwards to `info`. Output longer than 256 bytes is silently truncated.

The format string is checked against its arguments at compile time, so `info_f("%d", 2.5f)` — or passing a `std::string` where `%s` expects a `char*` — is a compiler warning, not a silent garbage read.

---

## Log::warn_f

### Syntax
```cpp
static void warn_f(const char* fmt, ...);
```

Same buffer / behavior as `info_f`; severity is warning.

---

## Log::error_f

### Syntax
```cpp
static void error_f(const char* fmt, ...);
```

Same buffer / behavior as `info_f`; severity is error.

## Example

```cpp
fjell::Log::info("ready");
fjell::Log::warn_f("low ammo: %d rounds left", ammo);
fjell::Log::error_f("missing asset: %s", path.c_str());
```

## See also
- [`Debug`](Debug.md) — visual logging in the 3D viewport, and on-screen debug text
