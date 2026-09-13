#pragma once

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

class SimTelemetryManager; // forward declaration

class DrivingAnalyzer {
public:
    static godot::Array analyze_lap(SimTelemetryManager* sim, const godot::Dictionary& lap, const godot::Dictionary& reference_lap = godot::Dictionary());

private:
    static godot::Array check_coasting(SimTelemetryManager* sim, const godot::Dictionary& lap, const godot::Dictionary& reference_lap);
    static godot::Array check_abs_abuse(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_trail_braking(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_shift_duration_and_downshift(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_over_slowing(SimTelemetryManager* sim, const godot::Dictionary& lap, const godot::Dictionary& reference_lap);
    static godot::Array check_snap_oversteer(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_throttle_flutter(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_pedal_overlap(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_understeer(SimTelemetryManager* sim, const godot::Dictionary& lap);
    static godot::Array check_loss_of_control(SimTelemetryManager* sim, const godot::Dictionary& lap);
};

} // namespace godot
