#pragma once

#include "sim_provider.h"
#include "ac_data_structs.h"
#include "telemetry_file.h"
#include <godot_cpp/classes/node.hpp>
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

namespace godot {

class ACProvider : public ISimProvider {
private:
    // shared memory handles
    HANDLE hMapPhysics;
    HANDLE hMapGraphic;
    HANDLE hMapStatic;

    // shared memory pointers
    AC_SPagePhysics* dataPhysics;
    AC_SPageGraphic* dataGraphic;
    AC_SPageStatic* dataStatic;

    // logging settings
    double sample_interval = 0.02; // 50 hz
    double samples_per_meter = 1.0;
    
    // state
    bool is_connected_flag = false;
    std::atomic<bool> is_logging{false};
    std::atomic<bool> game_disconnected{false};
    
    int last_lap_count = 0;
    int last_physics_packet_id = -1;
    int last_i_current_time = 0;
    double last_recorded_meter = -1.0;
    String session_output_file_path = "";

    // data
    std::vector<AC_LapDataChannels> sessions_data;
    
    // loaded session data
    std::vector<AC_LapDataChannels> loaded_session_data;
    AC_SPageStatic loaded_session_static_data;
    double loaded_session_sample_interval = 0.0;
    double loaded_session_samples_per_meter = 0.0;
    int loaded_session_lap_count = -1;
    std::vector<uint64_t> loaded_session_lap_offsets;

    std::function<void(godot::String)> auto_save_callback;

    String save_file_signature = "ACTL";

    std::map<int, int> session_type_counts;

    std::thread logging_thread;
    std::mutex data_mutex;

    CompressionMethod compression_method = COMPRESSION_ZSTD;

    void logging_loop();
    String _open_session_file(const String& file_path, TelemetryFile& infile, AC_SPageStatic& out_static, double& out_sample_interval, double& out_samples_per_meter, uint64_t& out_lap_count, std::vector<uint64_t>& out_lap_offsets, int64_t& out_timestamp);
    Dictionary _calculate_session_metadata(const AC_SPageStatic& stat, uint64_t count, const std::vector<AC_LapDataChannels>& laps);
    Dictionary _static_to_dict(const AC_SPageStatic &s);
    Dictionary _lap_to_dict(const AC_LapDataChannels& c);

    void _flush_sessions_to_disk(std::vector<AC_LapDataChannels> data_to_save, AC_SPageStatic static_data_copy, double save_interval, double save_spm, String path);
    String _get_session_suffix(int session_enum);

    void apply_math_conversions_in_place(AC_LapDataChannels& lap);

public:
    ACProvider();
    virtual ~ACProvider() override;
    
    static bool check_is_active();
    
    // ISimProvider impl
    virtual godot::String connect_provider() override;
    virtual void disconnect_provider() override;
    virtual bool is_connected() const override { return is_connected_flag; }
    virtual int get_provider_status() const override;
    virtual void update() override;
    virtual void set_auto_save_callback(std::function<void(godot::String)> callback) override;
    virtual godot::String start_capture(const godot::String& output_file_path) override;
    virtual godot::String stop_capture(const godot::String& output_file_path = "") override;
    virtual bool is_logging_active() const override;

    virtual void set_sample_interval(double interval) override { if (!is_logging) sample_interval = interval; }
    virtual void set_samples_per_meter(double spm) override { if (!is_logging) samples_per_meter = spm; }
    virtual godot::String get_save_file_signature() const override { return save_file_signature; }
    
    virtual void set_compression_method(CompressionMethod method) override { compression_method = method; }
    virtual CompressionMethod get_compression_method() const override { return compression_method; }

    virtual godot::String load_session(const godot::String& file_path) override;
    virtual void close_session() override;
    virtual godot::Dictionary get_session_metadata() override;
    virtual godot::Dictionary get_session_metadata_from_file(const godot::String& file_path) override;
    virtual godot::Dictionary get_live_static_data() override;
    virtual godot::Dictionary get_live_snapshot() override;
    virtual godot::Dictionary get_lap_data(int lap_index) override;
    
    virtual int get_loaded_session_lap_count() override { return loaded_session_lap_count; }
    virtual godot::Dictionary get_loaded_session_static_data() override;
    virtual double get_loaded_session_sample_interval() override { return loaded_session_sample_interval; }
    virtual double get_loaded_session_samples_per_meter() override { return loaded_session_samples_per_meter; }
    virtual double get_loaded_session_total_fuel_consumption() override;
    virtual double get_loaded_session_lap_fuel_consumption(int lap_index) override;
    virtual double get_loaded_session_total_laps() override;
    virtual godot::Dictionary get_loaded_session_lap_stats(int lap_index) override;
    virtual godot::Dictionary calculate_lap_time_delta(const godot::String& target_file_path, int target_lap_index, const godot::String& current_file_path, int current_lap_index, const godot::PackedFloat32Array& reference_positions) override;

    virtual godot::String get_internal_channel_name(const godot::String& standard_name) override;


};

} // namespace godot
