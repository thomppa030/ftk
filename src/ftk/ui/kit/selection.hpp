#pragma once

// What is picked in a list, a tree or a grid, by one model everywhere: a
// click picks one alone, Ctrl-click adds or takes away one, Shift-click
// picks the run from the last one clicked to it, in the order shown, and a
// box dragged over them picks, adds or takes away what it covers. The one
// picked last is the primary, which what acts on a single item shows.
//
//     ui::Selection<uint32_t> picked;
//     if (ui::item_clicked()) picked.click(id, ui::modifiers(), shown_ids);
//     const bool lit = picked.contains(id);

#include "ftk/ui/core/input.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace ftk::ui {

template <typename Key>
class Selection {
public:
    /// A click on `key` among `shown`, the items in the order they are
    /// shown: alone, with Ctrl added or taken away, with Shift the run
    /// from the last one clicked to it.
    void click(const Key& key, Modifiers modifiers, std::span<const Key> shown) {
        if (modifiers.ctrl) {
            toggle(key);
        } else if (modifiers.shift) {
            range(key, shown);
        } else {
            select(key);
        }
    }

    /// `key` alone.
    void select(const Key& key) {
        clear();
        add(key);
    }

    /// `key` added, or taken away when it is picked; the run's anchor
    /// either way.
    void toggle(const Key& key) {
        if (!contains(key)) {
            add(key);
            return;
        }
        set_.erase(key);
        order_.erase(std::find(order_.begin(), order_.end(), key));
        // The primary goes to the one picked last of those left.
        if (primary_ == key) primary_ = order_.empty() ? std::nullopt : std::optional<Key>(order_.back());
        anchor_ = key;
    }

    /// Everything from the anchor, the last one clicked, to `key` in the
    /// order `shown`, and only that; `key` alone when either is not shown.
    /// The anchor stays, so a further Shift-click runs from it again.
    void range(const Key& key, std::span<const Key> shown) {
        const auto from = anchor_ ? std::find(shown.begin(), shown.end(), *anchor_) : shown.end();
        const auto to = std::find(shown.begin(), shown.end(), key);
        if (from == shown.end() || to == shown.end()) {
            select(key);
            return;
        }
        const std::optional<Key> anchor = anchor_;
        set_.clear();
        order_.clear();
        const auto [first, last] = from < to ? std::pair{from, to} : std::pair{to, from};
        for (auto it = first; it <= last; ++it) {
            set_.insert(*it);
            order_.push_back(*it);
        }
        primary_ = key;
        anchor_ = anchor;
    }

    /// What a box dragged over the items does, by the keys held: what it
    /// `took` becomes what is picked, the one `nearest` where the box began
    /// the primary; with Shift it is added, the primary staying (`nearest`
    /// when there was none); with Ctrl it is taken away (Ctrl wins over
    /// Shift). A plain box that took nothing leaves nothing picked.
    void boxed(std::span<const Key> took, const std::optional<Key>& nearest, Modifiers modifiers) {
        if (modifiers.ctrl) {
            keep_if([&](const Key& key) { return std::find(took.begin(), took.end(), key) == took.end(); });
            return;
        }
        const std::optional<Key> kept_primary = modifiers.shift ? primary_ : std::nullopt;
        if (!modifiers.shift) clear();
        for (const Key& key : took) {
            if (key != nearest) add(key);
        }
        if (nearest) add(*nearest);
        if (kept_primary) add(*kept_primary);
    }

    /// `key` added, made the primary and the anchor.
    void add(const Key& key) {
        if (set_.insert(key).second) order_.push_back(key);
        primary_ = key;
        anchor_ = key;
    }

    void clear() {
        set_.clear();
        order_.clear();
        primary_.reset();
        anchor_.reset();
    }

    /// Only what `keep` says stays: what is still there after a list is
    /// read again.
    template <typename Keep>
    void keep_if(Keep&& keep) {
        std::erase_if(order_, [&](const Key& key) { return !keep(key); });
        std::erase_if(set_, [&](const Key& key) { return !keep(key); });
        if (primary_ && !set_.contains(*primary_)) {
            primary_ = order_.empty() ? std::nullopt : std::optional<Key>(order_.back());
        }
        if (anchor_ && !keep(*anchor_)) anchor_.reset();
    }

    [[nodiscard]] bool contains(const Key& key) const { return set_.contains(key); }
    [[nodiscard]] bool empty() const { return order_.empty(); }
    [[nodiscard]] std::size_t size() const { return order_.size(); }

    /// The one picked last, which what acts on a single item shows.
    [[nodiscard]] const std::optional<Key>& primary() const { return primary_; }

    /// What is picked, in the order it was picked.
    [[nodiscard]] std::span<const Key> keys() const { return order_; }

private:
    std::unordered_set<Key> set_;
    std::vector<Key> order_;
    std::optional<Key> primary_;
    std::optional<Key> anchor_;
};

} // namespace ftk::ui
