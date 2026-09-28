// water_component.hpp — A body of water, as a script reaches it
// Shipped to game projects alongside fjell_api.h.
#pragma once

#include "fjell/api.h"

extern "C" {
FJELL_SDK_EXPORT float fjell_water_bed_agitation(const void* water, float depth);
FJELL_SDK_EXPORT float fjell_water_wave_height(const void* water);
FJELL_SDK_EXPORT float fjell_water_advance_silt(float load, float agitation, float dt);
FJELL_SDK_EXPORT float fjell_water_silt_reference_depth();
}

namespace fjell {

/// A body of water. A script reaches it through a `Ref<WaterComponent>` or
/// `get_component<WaterComponent>()`, and never holds one by value: it is the
/// engine's component itself, and everything about it goes through its
/// methods. A node is water when it carries one.
///
/// What a game asks a sea is not what it is made of but what it is doing:
/// how hard it is working the bed, how big it has got. The wave field itself
/// stays engine-side — a script that wanted to read sixteen trains would be
/// reimplementing the evaluator, and the two halves of that formula are
/// pinned together by a test for good reason.
///
/// Every call reads the *effective* field, the sea after the weather's wind
/// response, not the one authored on disk. That is the sea the player is
/// looking at.
///
/// @code
/// // Cloud the water in a storm and let it settle slowly afterwards.
/// void tick(float dt) override {
///     const float agitation = sea->bed_agitation(bay_depth_);   // [[fj::property]] Ref<WaterComponent> sea
///     silt_ = fjell::WaterComponent::advance_silt(silt_, agitation, dt);
///     fjell::material::set_param(sea.node(), "silt_load", silt_);
/// }
/// @endcode
struct WaterComponent {
    static constexpr const char* type_name = "WaterComponent";

    WaterComponent() = delete;
    WaterComponent(const WaterComponent&) = delete;
    WaterComponent& operator=(const WaterComponent&) = delete;

    /// How hard this sea is working a bed `depth` metres down, 0 (cannot
    /// reach it) to 1 (working it hard).
    ///
    /// Waves stir the bed by the orbital motion under them, which dies off
    /// exponentially with depth, so this is answered per train: wavelength
    /// decides the reach and amplitude how hard it works. A short chop is
    /// spent within a few metres while a long swell still moves a
    /// thirty-metre bed — which is why a storm clouds a coast and leaves the
    /// open sea clear.
    ///
    /// What sediment a script suspends from it is `advance_silt()`.
    ///
    /// @param depth Still-water depth in metres. Zero or less answers 0.
    [[nodiscard]] float bed_agitation(float depth) const { return fjell_water_bed_agitation(this, depth); }

    /// The sum of the sea's train amplitudes in metres, after the wind
    /// response: how big it has got. For a script deciding whether a boat
    /// should be taking spray, not for placing anything — where the surface
    /// actually is comes from buoyancy.
    [[nodiscard]] float wave_height() const { return fjell_water_wave_height(this); }

    /// One step of suspended sediment answering the sea working the bed.
    ///
    /// Sediment is slow in a way the weather is not: waves lift it in minutes
    /// and gravity returns it over the better part of an hour, so water does
    /// not go clear the moment a squall passes. Driving a clarity term from
    /// the wind directly is the same mistake as letting a gust heave the
    /// ocean — hold the load returned here and feed it back next frame.
    ///
    /// Frame-rate independent: the same curve at any `dt`.
    ///
    /// @param load      What is currently in suspension, 0 clear to 1.
    /// @param agitation What the sea is doing to the bed, from
    ///                  `bed_agitation()`.
    /// @return The new load, to keep and pass back next frame.
    [[nodiscard]] static float advance_silt(float load, float agitation, float dt) {
        return fjell_water_advance_silt(load, agitation, dt);
    }

    /// The depth a sea's stirring is conventionally quoted over, in metres.
    ///
    /// Only useful to a script that wants one load for a whole body of water
    /// and lets its material reweight per pixel from the depth buffer: ask
    /// `bed_agitation()` at this depth, push the load, and let the shader
    /// scale it by the depth it can see. A script that knows the depth it
    /// cares about should pass that instead.
    [[nodiscard]] static float silt_reference_depth() { return fjell_water_silt_reference_depth(); }
};

} // namespace fjell
