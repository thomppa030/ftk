#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <span>
#include <string_view>
#include <string>

namespace ftk {
class ThreadPool;
} // namespace ftk

namespace fjell {

class Project;

/// The drop-down an asset field opens when it is clicked: every asset the
/// field accepts, grouped under the project and the engine, each with the
/// thumbnail the content browser shows.
///
/// Drawn by asset_ref_field(); nothing else opens one. The listing behind it
/// is a directory walk, so it is taken when a picker opens and shared by
/// every field accepting the same extensions.
namespace asset_picker {

/// The pool the directory walk runs on. Called once at startup; without it
/// the walk runs on the calling thread and stalls the frame that opens a
/// picker, which a project of any size is felt as a hitch.
void set_thread_pool(ftk::ThreadPool* pool);

/// Open the picker for `widget_id` on the next draw. The caller has already
/// pushed its ImGui ID, so `widget_id` only has to tell two fields apart.
void open(const char* widget_id);

/// Draw the picker if it belongs to `widget_id`. Returns true and writes the
/// chosen project- or engine-relative path to `picked` when the user picks
/// one; the caller turns that into a reference. Shaders and materials are
/// listed only when drawn as one of `shader_types`; empty lists them all.
/// `selected` is the content browser's selection, project-relative, offered
/// first when the field takes it; empty for none.
bool draw(const char* widget_id, std::span<const std::string> extensions,
          std::span<const ShaderType> shader_types, const Project* project,
          std::string_view selected, std::string& picked);

/// Forget the cached listing, so the next picker walks the directories again.
/// For when assets are added or removed behind the editor's back.
void invalidate();

} // namespace asset_picker
} // namespace fjell
