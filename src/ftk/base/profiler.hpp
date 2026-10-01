#pragma once

#ifdef FTK_ENABLE_TRACY

#include <tracy/Tracy.hpp>

// CPU profiling
#define FTK_PROFILE_FRAME       FrameMark
#define FTK_PROFILE_SCOPE       ZoneScoped
#define FTK_PROFILE_SCOPE_N(name) ZoneScopedN(name)

// Dynamic-name CPU zone — label is computed at runtime (per-pass
// names during DAG declare/submit, per-viewport IDs, etc). Costs a
// strlen() per frame; prefer FTK_PROFILE_SCOPE_N for fixed names.
#define FTK_PROFILE_SCOPE_DYNAMIC(name_cstr) \
    ZoneTransientN(___tracy_cpu_zone_transient, name_cstr, true)

// Dynamic-name CPU zone from a std::string_view, which need not be
// null-terminated. Same per-frame cost as FTK_PROFILE_SCOPE_DYNAMIC
// without the strlen().
#define FTK_PROFILE_SCOPE_VIEW(name_view)                                        \
    tracy::ScopedZone ___tracy_cpu_zone_view(                                      \
        TracyLine, TracyFile, strlen(TracyFile), TracyFunction,                    \
        strlen(TracyFunction), (name_view).data(), (name_view).size(), -1, true)

#else

#define FTK_PROFILE_FRAME         (void)0
#define FTK_PROFILE_SCOPE         (void)0
#define FTK_PROFILE_SCOPE_N(name) (void)0
#define FTK_PROFILE_SCOPE_DYNAMIC(name) (void)0
#define FTK_PROFILE_SCOPE_VIEW(name)    (void)(name)

#endif
