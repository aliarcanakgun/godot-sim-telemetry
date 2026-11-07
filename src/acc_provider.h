#pragma once

#include "sim_provider.h"
#include "acc_data_structs.h"
#include "telemetry_file.h"
#include <godot_cpp/classes/node.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <windows.h>

namespace godot {

class ACCProvider : public ISimProvider {
private:
    // shared memory handles
    HANDLE hMapPhysics;
    HANDLE hMapGraphic;
    HANDLE hMapStatic;

    // shared memory pointers
    ACC_SPagePhysics* dataPhysics;
    ACC_SPageGraphic* dataGraphic;
    ACC_SPageStatic* dataStatic;

    // logging settings
    double samples_per_meter = 1.0;
    
    // state
    bool is_connected_flag = false;
    std::atomic<bool> is_logging{false};
    std::atomic<bool> game_disconnected{false};
    
    int last_lap_count = 0;
    int last_physics_packet_id = -1;
    int last_i_current_time = 0;
    int last_player_car_index = 0;
    double last_recorded_meter = -1.0;
    String session_output_file_path = "";

    // data
    std::vector<ACC_LapDataChannels> sessions_data;
    
    // loaded session state
    struct LapSummary {
        int snapshot_count = 0;
        int timestamp_count = 0;
        bool has_positions = false;
        size_t pos_count = 0;
        float first_pos = 0.0f;
        float last_pos = 0.0f;
        float total_progression = 0.0f;
        float top_speed = 0.0f;
        int stats_end_sec_idx = 0;
        Dictionary meta_sector_times;
        Dictionary stats_sector_times;
        Dictionary sectors_data;
        bool has_invalid_flag = false;
        bool has_iCurrentTime = false;
        int max_iCurrentTime = 0;
        int back_iCurrentTime = 0;
        bool has_iLastTime = false;
        int most_freq_iLastTime = 0;
        int max_iLastTime = 0;
        int first_sec0_last_sector_time = 0;
        double fuel_consumed = 0.0;
    };

    String loaded_session_file_path = "";
    ACC_SPageStatic loaded_session_static_data{};
    double loaded_session_samples_per_meter = 0.0;
    int loaded_session_lap_count = -1;
    std::vector<uint64_t> loaded_session_lap_offsets;
    std::vector<uint64_t> loaded_visible_lap_offsets;

    bool summaries_loaded = false;
    std::vector<LapSummary> loaded_lap_summaries;
    double loaded_total_driven_laps = 0.0;
    int loaded_session_type = -1;

    int cached_lap_index = -1;
    ACC_LapDataChannels cached_lap;

    std::function<void(godot::String)> auto_save_callback;

    String save_file_signature = "ACCT";
    
    std::map<int, int> session_type_counts;

    std::thread logging_thread;
    std::mutex data_mutex;

    CompressionMethod compression_method = COMPRESSION_ZSTD;

    void logging_loop();
    String _open_session_file(const String& file_path, TelemetryFile& infile, ACC_SPageStatic& out_static, double& out_samples_per_meter, uint64_t& out_lap_count, std::vector<uint64_t>& out_lap_offsets, int64_t& out_timestamp);
    void _unwrap_lap_positions(ACC_LapDataChannels& lap) const;
    bool _scan_session_summaries(const String& file_path, ACC_SPageStatic& out_static, double& out_spm, uint64_t& out_lap_count, int64_t& out_timestamp, std::vector<LapSummary>& out_summaries, double& out_total_driven, int& out_session_type);
    bool _ensure_summaries_loaded();
    Dictionary _build_session_metadata(const ACC_SPageStatic& stat, uint64_t count, int session_type, const std::vector<LapSummary>& summaries) const;
    Dictionary _static_to_dict(const ACC_SPageStatic &s);
    Dictionary _lap_to_dict(const ACC_LapDataChannels& c);

    void _flush_sessions_to_disk(std::vector<ACC_LapDataChannels> data_to_save, ACC_SPageStatic static_data_copy, double save_spm, String path);
    String _get_session_suffix(int session_enum);

    void apply_math_conversions_in_place(ACC_LapDataChannels& lap);

public:
    ACCProvider();
    virtual ~ACCProvider() override;
    
    static bool check_is_active();
    
    float get_acc_bbias_offset(const String& car_model) const;
    float get_acc_bpressure_multiplier(const String& car_model, const bool is_front) const;
    float get_acc_track_length(const String& track_name) const;
    std::vector<float> get_acc_sectors(const String& track_name) const;

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
    virtual double get_loaded_session_samples_per_meter() override { return loaded_session_samples_per_meter; }
    virtual double get_loaded_session_total_fuel_consumption() override;
    virtual double get_loaded_session_lap_fuel_consumption(int lap_index) override;
    virtual double get_loaded_session_total_laps() override;
    virtual godot::Dictionary get_loaded_session_lap_stats(int lap_index) override;
    virtual godot::Dictionary calculate_lap_time_delta(const godot::String& target_file_path, int target_lap_index, const godot::String& current_file_path, int current_lap_index, const godot::PackedFloat32Array& reference_positions) override;

    virtual godot::String get_internal_channel_name(const godot::String& standard_name) override;
};

} // namespace godot
