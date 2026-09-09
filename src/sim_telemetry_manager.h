#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <memory>
#include "sim_provider.h"
#include "gd_lap_telemetry.h"
#include "telemetry_enums.h"

namespace godot {

class SimTelemetryManager : public Node {
    GDCLASS(SimTelemetryManager, Node)

private:
    std::unique_ptr<ISimProvider> active_provider;
    String current_sim_id = "";
    
    // properties
    double sample_interval = 0.02;
    double samples_per_meter = 1.0;
    CompressionMethod default_compression_method = COMPRESSION_ZSTD;

    ISimProvider* _get_provider_for_signature(const String& sig);
    ISimProvider* _create_provider(const String& sim_id);
    String _detect_file_signature(const String& file_path);

protected:
    static void _bind_methods();

public:
    SimTelemetryManager();
    ~SimTelemetryManager();

    virtual void _process(double delta) override;

    // connections
    String connect_to_sim(const String& sim_id);
    String detect_active_sim() const;
    void disconnect_from_sim();
    bool is_connected_to_sim() const;
    String get_current_sim_id() const;

    // logging
    String start_logging(const String& output_file_path);
    String finish_logging(const String& output_file_path = "");
    bool is_currently_logging() const;

    // live data
    Dictionary get_live_static_data();
    Ref<GDLapTelemetry> get_live_snapshot();
    int get_sim_status();

    // loading
    String load_session_data(const String& file_path);
    Dictionary get_session_metadata(const String& file_path);
    Dictionary get_loaded_session_metadata();
    void close_loaded_session();
    
    int get_loaded_session_lap_count();
    Ref<GDLapTelemetry> get_loaded_session_lap_data(int lap_index);
    Dictionary get_loaded_session_static_data();
    double get_loaded_session_sample_interval();
    double get_loaded_session_samples_per_meter();
    double get_loaded_session_total_fuel_consumption();
    double get_loaded_session_lap_fuel_consumption(int lap_index);
    double get_loaded_session_total_laps();
    Dictionary get_loaded_session_lap_stats(int lap_index);
    Dictionary calculate_lap_time_delta(const String& target_file_path, int target_lap_index = 0, const String& current_file_path = "", int current_lap_index = 0, const PackedFloat32Array& reference_positions = PackedFloat32Array());

    // analysis
    Array analyze_lap(const Dictionary& lap_data, const Dictionary& reference_lap_data = Dictionary());

    // getters/setters
    double get_sample_interval() const { return sample_interval; }
    void set_sample_interval(double p_sample_interval) { 
        if (p_sample_interval <= 0.0) return;
        if (!is_currently_logging()) {
            sample_interval = p_sample_interval; 
            if (active_provider) active_provider->set_sample_interval(sample_interval);
        }
    }

    double get_samples_per_meter() const { return samples_per_meter; }
    void set_samples_per_meter(double p_samples_per_meter) { 
        if (p_samples_per_meter <= 0.0) return;
        if (!is_currently_logging()) {
            samples_per_meter = p_samples_per_meter; 
            if (active_provider) active_provider->set_samples_per_meter(samples_per_meter);
        }
    }

    String get_save_file_signature() const { 
        if (active_provider) return active_provider->get_save_file_signature();
        return "";
    }
    
    void set_default_compression_method(CompressionMethod method);
    CompressionMethod get_default_compression_method() const;
    
    String get_compression_format(const String& file_path);
    bool compress_file(const String& file_path, CompressionMethod method);
    void compress_file_async(const String& file_path, CompressionMethod method);
    bool uncompress_file(const String& file_path);
    void uncompress_file_async(const String& file_path);

    // utils
    Vector2 get_array_min_max(const Variant& arr);
    String get_channel_name(const String& standard_name);
};

} // namespace godot

VARIANT_ENUM_CAST(SimStatusType);
VARIANT_ENUM_CAST(SimSessionType);
VARIANT_ENUM_CAST(SimFlagType);
VARIANT_ENUM_CAST(DrivingMistakeType);
VARIANT_ENUM_CAST(CompressionMethod);
