#include "ftk/ui/core/context.hpp"

#include "ftk/base/log.hpp"

#include <algorithm>
#include <cassert>

namespace ftk::ui::core {

namespace {

Context* current_context = nullptr;

} // namespace

void Context::begin_frame(double time) {
    if (in_frame_) {
        FTK_CORE_ERROR("UI: a frame began before the last one ended");
        (void)end_frame();
    }
    delta_ = frame_ == 0 ? 0.0f : static_cast<float>(time - time_);
    time_ = time;
    ++frame_;
    in_frame_ = true;
    next_now_ = false;
    next_at_.reset();
    output_.roots.clear();
    output_.next = {};
}

void Context::begin_root(RootId root, glm::vec2 size, float scale) {
    assert(in_frame_ && "UI: a root begun outside a frame");
    if (root_) {
        FTK_CORE_ERROR("UI: window {} began inside window {}", root, root_->id);
        end_root();
    }
    root_ = Root{.id = root, .size = size, .scale = scale};
}

void Context::end_root() {
    if (!root_) {
        FTK_CORE_ERROR("UI: a window ended that never began");
        return;
    }
    // Ids pushed and never popped would shift every id after them.
    if (ids_.depth() != 0) {
        FTK_CORE_ERROR("UI: window {} ended with {} id(s) still pushed", root_->id, ids_.depth());
        ids_.clear();
    }
    output_.roots.push_back({.root = root_->id, .size = root_->size, .scale = root_->scale});
    root_.reset();
}

const FrameOutput& Context::end_frame() {
    if (root_) {
        FTK_CORE_ERROR("UI: the frame ended inside window {}", root_->id);
        end_root();
    }
    in_frame_ = false;
    // A deadline already reached is no reason to sleep.
    if (next_now_ || (next_at_ && *next_at_ <= time_)) {
        output_.next.now = true;
    } else {
        output_.next.at = next_at_;
    }
    return output_;
}

void Context::request_frame_at(double time) {
    next_at_ = next_at_ ? std::min(*next_at_, time) : time;
}

Context& current() {
    assert(current_context != nullptr && "UI: no context set; call core::set_current() first");
    return *current_context;
}

void set_current(Context* context) {
    current_context = context;
}

} // namespace ftk::ui::core
