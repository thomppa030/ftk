#include "renderer/pass/render_pass.hpp"

namespace fjell {

// RenderPass is a pure interface for declare()/record(); its methods
// have inline defaults in the header. This TU exists to anchor the
// vtable and give room for future non-inline members.

} // namespace fjell
