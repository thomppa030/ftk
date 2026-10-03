#include "ftk/ui/core/layout.hpp"

#include "ftk/base/log.hpp"

#include <algorithm>

namespace ftk::ui::core {

void Layout::reset() {
    scopes_.clear();
    last_item_ = {};
}

void Layout::begin_area(Rect bounds, glm::vec2 scroll) {
    Scope area;
    area.kind = Kind::Area;
    area.bounds = bounds;
    area.start = bounds.min - scroll;
    area.line_start_x = area.start.x;
    area.cursor = area.start;
    area.extent = {.min = area.start, .max = area.start};
    area.item_before = last_item_;
    scopes_.push_back(area);
}

glm::vec2 Layout::end_area() {
    if (scopes_.empty() || top().kind != Kind::Area) {
        FTK_CORE_ERROR("UI: an area ended inside a group or container, or never began");
        return {0.0f, 0.0f};
    }
    const Scope area = top();
    scopes_.pop_back();
    last_item_ = area.item_before;
    return area.has_items ? area.extent.max - area.start : glm::vec2{0.0f, 0.0f};
}

Layout::Position Layout::position(const Scope& scope, const glm::vec2* size) const {
    const bool rows = scope.kind != Kind::Container || scope.options.wrap;
    if (rows) {
        // A wrapping row puts each child after the last when it fits.
        const bool wrapping = scope.kind == Kind::Container;
        if (scope.has_items && (scope.same_line || wrapping)) {
            const float gap = wrapping ? scope.gap : scope.same_line_gap;
            const float x = scope.prev_end_x + gap;
            const bool fits = !wrapping || size == nullptr || x + size->x <= scope.bounds.max.x;
            if (fits) return {.at = {x, scope.prev_line_top}, .continues_line = true};
            return {.at = {scope.line_start_x, scope.prev_line_top + scope.prev_line_height + style_.item_spacing.y}};
        }
        return {.at = scope.cursor};
    }
    // Along the axis from where the group is; across it, centred when asked,
    // by its own size or, unknown yet, by the children's last frame.
    const float across_size = size != nullptr ? (scope.options.axis == Axis::Row ? size->y : size->x)
                                              : (scope.memory != nullptr ? scope.memory->across : 0.0f);
    if (scope.options.axis == Axis::Row) {
        float y = scope.start.y;
        if (scope.options.centre_across && scope.memory != nullptr) {
            y += std::max(0.0f, 0.5f * (scope.memory->across - across_size));
        }
        return {.at = {scope.along, y}};
    }
    float x = scope.start.x;
    if (scope.options.centre_across) x += std::max(0.0f, 0.5f * (scope.bounds.width() - across_size));
    return {.at = {x, scope.along}};
}

void Layout::commit(Scope& scope, Rect rect, bool continues_line) {
    last_item_ = rect;
    scope.extent = scope.has_items ? scope.extent.merge(rect) : rect;
    scope.has_items = true;
    if (scope.kind != Kind::Container || scope.options.wrap) {
        const float line_height = continues_line ? std::max(scope.prev_line_height, rect.height()) : rect.height();
        scope.prev_end_x = rect.max.x;
        scope.prev_line_top = rect.min.y;
        scope.prev_line_height = line_height;
        scope.cursor = {scope.line_start_x, rect.min.y + line_height + style_.item_spacing.y};
        scope.same_line = false;
        return;
    }
    const bool row = scope.options.axis == Axis::Row;
    const float begin = row ? rect.min.x : rect.min.y;
    const float end = row ? rect.max.x : rect.max.y;
    if (!scope.group_has_items) scope.group_begin = begin;
    scope.group_end = std::max(scope.group_has_items ? scope.group_end : end, end);
    scope.group_has_items = true;
    scope.along = end + scope.gap;
    scope.measured.across = std::max(scope.measured.across, row ? rect.height() : rect.width());
}

Rect Layout::next(glm::vec2 size) {
    Scope& scope = top();
    const Position at = position(scope, &size);
    const Rect rect{.min = at.at, .max = at.at + size};
    commit(scope, rect, at.continues_line);
    return rect;
}

void Layout::same_line(float gap) {
    Scope& scope = top();
    if (scope.kind == Kind::Container) return;  // a container places its own
    scope.same_line = true;
    scope.same_line_gap = gap < 0.0f ? style_.item_spacing.x : gap;
}

void Layout::new_line() {
    Scope& scope = top();
    if (scope.kind == Kind::Container) return;
    if (scope.same_line) {
        scope.same_line = false;
        return;
    }
    scope.cursor.y += style_.empty_line;
}

void Layout::spacing(float amount) {
    Scope& scope = top();
    const float room = amount < 0.0f ? style_.item_spacing.y : amount;
    if (scope.kind == Kind::Container && !scope.options.wrap) {
        scope.along += room;
        return;
    }
    scope.same_line = false;
    scope.cursor.y += room;
}

glm::vec2 Layout::cursor() const {
    return position(top(), nullptr).at;
}

void Layout::set_cursor(glm::vec2 at) {
    Scope& scope = top();
    if (scope.kind == Kind::Container && !scope.options.wrap) {
        scope.along = scope.options.axis == Axis::Row ? at.x : at.y;
        return;
    }
    scope.cursor = at;
    scope.same_line = false;
}

glm::vec2 Layout::space_left() const {
    return glm::max(top().bounds.max - cursor(), glm::vec2{0.0f, 0.0f});
}

float Layout::resolve_width(float width) const {
    const float room = space_left().x;
    if (width == FILL) return room;
    if (width < 0.0f) return std::max(room + width, 0.0f);
    return width;
}

Rect Layout::area() const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (scope->kind == Kind::Area) return scope->bounds;
    }
    return {};
}

void Layout::begin_group() {
    const Scope& parent = top();
    const Position at = position(parent, nullptr);
    Scope group;
    group.kind = Kind::Group;
    group.bounds = parent.bounds;
    group.start = at.at;
    group.continues_line = at.continues_line;
    group.line_start_x = at.at.x;
    group.cursor = at.at;
    group.extent = {.min = at.at, .max = at.at};
    scopes_.push_back(group);
}

Rect Layout::end_group() {
    if (scopes_.size() < 2 || top().kind != Kind::Group) {
        FTK_CORE_ERROR("UI: a group ended that isn't the innermost scope");
        return {};
    }
    const Scope group = top();
    scopes_.pop_back();
    const Rect rect = group.has_items ? Rect{.min = group.start, .max = group.extent.max}
                                      : Rect{.min = group.start, .max = group.start};
    commit(top(), rect, group.continues_line);
    return rect;
}

float Layout::group_start(const Scope& scope, std::size_t group) const {
    float at = scope.options.axis == Axis::Row ? scope.start.x : scope.start.y;
    for (std::size_t i = 0; i < group; ++i) {
        const bool known = scope.memory != nullptr && i < scope.memory->group_count;
        at += (known ? scope.memory->groups[i] : 0.0f) + scope.spring;
    }
    return at;
}

void Layout::begin_container(const ContainerOptions& options, const ContainerMemory* memory) {
    const Scope& parent = top();
    const Position at = position(parent, nullptr);
    Scope container;
    container.kind = Kind::Container;
    container.options = options;
    if (options.wrap) container.options.axis = Axis::Row;
    container.memory = options.wrap ? nullptr : memory;
    const bool row = container.options.axis == Axis::Row;
    container.gap = options.gap >= 0.0f ? options.gap : (row ? style_.item_spacing.x : style_.item_spacing.y);
    container.bounds = {.min = at.at, .max = parent.bounds.max};
    container.start = at.at;
    container.continues_line = at.continues_line;
    container.line_start_x = at.at.x;
    container.cursor = at.at;
    container.extent = {.min = at.at, .max = at.at};
    container.along = row ? at.at.x : at.at.y;
    // The room the groups didn't take last frame, shared between the
    // springs between them; never less than the gap.
    container.spring = container.gap;
    if (container.memory != nullptr && container.memory->group_count > 1) {
        float taken = 0.0f;
        for (uint8_t i = 0; i < container.memory->group_count; ++i) taken += container.memory->groups[i];
        const float length = row ? container.bounds.width() : container.bounds.height();
        const auto springs = static_cast<float>(container.memory->group_count - 1);
        container.spring = std::max(container.gap, (length - taken) / springs);
    }
    scopes_.push_back(container);
}

void Layout::close_group(Scope& scope) {
    const float length = scope.group_has_items ? scope.group_end - scope.group_begin : 0.0f;
    ContainerMemory& measured = scope.measured;
    if (measured.group_count < MAX_CONTAINER_GROUPS) {
        measured.groups[measured.group_count++] = length;
    } else {
        measured.groups[MAX_CONTAINER_GROUPS - 1] += scope.spring + length;
    }
    scope.group_has_items = false;
}

void Layout::spring() {
    Scope& scope = top();
    if (scope.kind != Kind::Container || scope.options.wrap) {
        FTK_CORE_ERROR("UI: a spring outside a container, or in a wrapping row");
        return;
    }
    close_group(scope);
    scope.sprung = true;
    scope.along = group_start(scope, scope.measured.group_count);
}

Rect Layout::end_container(ContainerMemory& measured) {
    if (scopes_.size() < 2 || top().kind != Kind::Container) {
        FTK_CORE_ERROR("UI: a container ended that isn't the innermost scope");
        return {};
    }
    Scope container = top();
    scopes_.pop_back();
    close_group(container);
    Rect rect{.min = container.start, .max = container.has_items ? container.extent.max : container.start};
    if (!container.options.wrap) {
        const ContainerMemory& kept = container.measured;
        if (container.options.axis == Axis::Row) {
            if (container.sprung) rect.max.x = container.bounds.max.x;
            rect.max.y = container.start.y + kept.across;
        } else {
            if (container.sprung) rect.max.y = container.bounds.max.y;
            rect.max.x = container.options.centre_across ? container.bounds.max.x : container.start.x + kept.across;
        }
    }
    measured = container.measured;
    commit(top(), rect, container.continues_line);
    return rect;
}

} // namespace ftk::ui::core
