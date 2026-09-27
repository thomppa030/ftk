#pragma once

namespace fjell {

/// Disconnect every input binding `owner` made through input::bind. For a
/// script instance or widget controller destroyed while play goes on: its
/// bindings hold its pointer and would otherwise call into it after it is
/// gone. Play start, play stop and a script reload clear every binding at
/// once instead.
void release_input_bindings(const void* owner);

} // namespace fjell
