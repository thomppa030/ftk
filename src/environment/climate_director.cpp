#include "environment/climate_director.hpp"

#include <algorithm>

namespace fjell {

namespace {

/// A hash good enough to stand in for a random number generator, and exactly
/// reproducible on every machine: no library RNG whose stream could differ
/// between standard libraries sits between the seed and the weather.
uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B9u + b + 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float unit_float(uint32_t h) {
    return static_cast<float>(h >> 8) / static_cast<float>(1U << 24);
}

} // namespace

void ClimateDirector::set_climate(const ClimateAsset* climate, float hour) {
    climate_ = climate;
    current_ = {};
    forecast_.clear();
    ordinal_ = 0;
    if (climate_ == nullptr || climate_->states.empty()) return;

    int start = climate_->index_of(climate_->initial);
    if (start < 0) start = 0;
    current_ = ClimateRoll{.state_index = start, .ordinal = 0};
    update_forecast(hour);
}

void ClimateDirector::resume(const ClimateAsset* climate, uint32_t ordinal, float hour) {
    set_climate(climate, hour);
    if (climate_ == nullptr || ordinal == 0) return;
    // Replay the sequence from the start: each roll follows the last, so the
    // ordinal alone names a weather only by walking there.
    while (ordinal_ < ordinal) {
        const ClimateRoll next = advance(hour);
        if (!next.valid()) break;
    }
}

int ClimateDirector::roll_state(int from, uint32_t ordinal, float hour) const {
    if (climate_ == nullptr || climate_->states.empty()) return -1;
    const ClimateState* current = from >= 0 ? &climate_->states[static_cast<size_t>(from)] : nullptr;

    // The states this one allows, that may start at this hour, that have any
    // weight at all.
    float total = 0.0f;
    std::vector<std::pair<int, float>> allowed;
    for (size_t i = 0; i < climate_->states.size(); ++i) {
        const ClimateState& candidate = climate_->states[i];
        if (candidate.weight <= 0.0f) continue;
        if (current != nullptr && !current->next.empty()) {
            if (std::find(current->next.begin(), current->next.end(), candidate.name) == current->next.end()) {
                continue;
            }
        }
        if (!candidate.allows_hour(hour)) continue;
        allowed.emplace_back(static_cast<int>(i), candidate.weight);
        total += candidate.weight;
    }
    if (allowed.empty() || total <= 0.0f) {
        // Nothing may follow at this hour: hold what plays rather than
        // picking something the climate forbids.
        return from;
    }

    float pick = unit_float(mix(climate_->seed, ordinal)) * total;
    for (const auto& [index, weight] : allowed) {
        pick -= weight;
        if (pick <= 0.0f) return index;
    }
    return allowed.back().first;
}

ClimateRoll ClimateDirector::advance(float hour) {
    if (climate_ == nullptr || held_) return {};
    ++ordinal_;
    const int next = roll_state(current_.state_index, ordinal_, hour);
    current_ = ClimateRoll{.state_index = next, .ordinal = ordinal_};
    update_forecast(hour);
    return current_;
}

void ClimateDirector::update_forecast(float hour) {
    forecast_.clear();
    if (climate_ == nullptr) return;
    int from = current_.state_index;
    for (uint32_t i = 1; i <= FORECAST_LENGTH; ++i) {
        const uint32_t ordinal = ordinal_ + i;
        const int next = roll_state(from, ordinal, hour);
        if (next < 0) break;
        forecast_.push_back(ClimateRoll{.state_index = next, .ordinal = ordinal});
        from = next;
    }
}

} // namespace fjell
