#include "ftk/ui/core/context.hpp"

#include "ftk/base/log.hpp"

#include <algorithm>
#include <cassert>

namespace ftk::ui::core {

namespace {

Context* current_context = nullptr;

} // namespace

Context::Context(text::AtlasOptions atlas) : text_{atlas} {}

Context::~Context() {
    if (current_context == this) current_context = nullptr;
}

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
    text_.begin_frame();
}

void Context::begin_root(RootId root, glm::vec2 size, float scale) {
    assert(in_frame_ && "UI: a root begun outside a frame");
    if (root_ != nullptr) {
        FTK_CORE_ERROR("UI: window {} began inside window {}", root, root_->id);
        end_root();
    }
    std::unique_ptr<Root>& kept = roots_[root];
    if (!kept) kept = std::make_unique<Root>();
    root_ = kept.get();
    root_->id = root;
    root_->size = size;
    root_->scale = scale;
    root_->list.reset(root_->draw, size, scale, &text_);
}

void Context::end_root() {
    if (root_ == nullptr) {
        FTK_CORE_ERROR("UI: a window ended that never began");
        return;
    }
    // Ids pushed and never popped would shift every id after them.
    if (ids_.depth() != 0) {
        FTK_CORE_ERROR("UI: window {} ended with {} id(s) still pushed", root_->id, ids_.depth());
        ids_.clear();
    }
    output_.roots.push_back({.root = root_->id, .size = root_->size, .scale = root_->scale, .draw = &root_->draw});
    root_ = nullptr;
}

void Context::forget_root(RootId root) {
    if (root_ != nullptr && root_->id == root) {
        FTK_CORE_ERROR("UI: window {} forgotten while it is drawn", root);
        return;
    }
    roots_.erase(root);
}

DrawList& Context::draw_list() {
    assert(root_ != nullptr && "UI: drawn outside a window; call begin_root() first");
    return root_->list;
}

const FrameOutput& Context::end_frame() {
    if (root_ != nullptr) {
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
