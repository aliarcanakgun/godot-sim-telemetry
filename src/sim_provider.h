#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <functional>
#include "telemetry_enums.h"

namespace godot {

class ISimProvider {
public:
    virtual ~ISimProvider() = default;

    virtual godot::String start_capture(const godot::String& output_file_path) = 0;
    virtual godot::String stop_capture(const godot::String& output_file_path = "") = 0;
    virtual bool is_logging_active() const = 0;

    virtual void set_sample_interval(double interval) = 0;
    virtual void set_samples_per_meter(double spm) = 0;
    virtual godot::String get_save_file_signature() const = 0;
    
    virtual void set_compression_method(CompressionMethod method) = 0;
    virtual CompressionMethod get_compression_method() const = 0;

    virtual godot::String connect_provider() = 0;
    virtual void disconnect_provider() = 0;
    virtual bool is_connected() const = 0;
    virtual int get_provider_status() const = 0;
    virtual void update() = 0;
    
    virtual void set_auto_save_callback(std::function<void(godot::String)> callback) = 0;
    
    virtual godot::String load_session(const godot::String& file_path) = 0;
    virtual void close_session() = 0;

    virtual godot::Dictionary get_session_metadata() = 0;
    virtual godot::Dictionary get_session_metadata_from_file(const godot::String& file_path) = 0;
    virtual godot::Dictionary get_live_static_data() = 0;
    virtual godot::Dictionary get_live_snapshot() = 0;
    virtual godot::Dictionary get_lap_data(int lap_index) = 0;
    
    virtual int get_loaded_session_lap_count() = 0;
    virtual godot::Dictionary get_loaded_session_static_data() = 0;
    virtual double get_loaded_session_sample_interval() = 0;
    virtual double get_loaded_session_samples_per_meter() = 0;
    virtual double get_loaded_session_total_fuel_consumption() = 0;
    virtual double get_loaded_session_lap_fuel_consumption(int lap_index) = 0;
    virtual double get_loaded_session_total_laps() = 0;
    virtual godot::Dictionary get_loaded_session_lap_stats(int lap_index) = 0;
    virtual godot::Dictionary calculate_lap_time_delta(const godot::String& target_file_path, int target_lap_index, const godot::String& current_file_path, int current_lap_index, const godot::PackedFloat32Array& reference_positions) = 0;

    virtual godot::String get_internal_channel_name(const godot::String& standard_name) = 0;
};

} // namespace godot
