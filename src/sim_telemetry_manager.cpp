#include "sim_telemetry_manager.h"
#include "ac_provider.h"
#include "acc_provider.h"
#include "driving_analyzer.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include "telemetry_file.h"
#include <cmath>
#include <windows.h>

using namespace godot;

SimTelemetryManager::SimTelemetryManager() {
    set_process(true);
}

SimTelemetryManager::~SimTelemetryManager() {
    disconnect_from_sim();
}

void SimTelemetryManager::_bind_methods() {
    BIND_ENUM_CONSTANT(STATUS_OFF);
    BIND_ENUM_CONSTANT(STATUS_REPLAY);
    BIND_ENUM_CONSTANT(STATUS_LIVE);
    BIND_ENUM_CONSTANT(STATUS_PAUSE);

    BIND_ENUM_CONSTANT(SESSION_UNKNOWN);
    BIND_ENUM_CONSTANT(SESSION_PRACTICE);
    BIND_ENUM_CONSTANT(SESSION_QUALIFY);
    BIND_ENUM_CONSTANT(SESSION_RACE);
    BIND_ENUM_CONSTANT(SESSION_HOTLAP);
    BIND_ENUM_CONSTANT(SESSION_TIME_ATTACK);
    BIND_ENUM_CONSTANT(SESSION_DRIFT);
    BIND_ENUM_CONSTANT(SESSION_DRAG);
    BIND_ENUM_CONSTANT(SESSION_HOTSTINT);
    BIND_ENUM_CONSTANT(SESSION_HOTSTINT_SUPERPOLE);

    BIND_ENUM_CONSTANT(FLAG_NONE);
    BIND_ENUM_CONSTANT(FLAG_BLUE);
    BIND_ENUM_CONSTANT(FLAG_YELLOW);
    BIND_ENUM_CONSTANT(FLAG_BLACK);
    BIND_ENUM_CONSTANT(FLAG_WHITE);
    BIND_ENUM_CONSTANT(FLAG_CHECKERED);
    BIND_ENUM_CONSTANT(FLAG_PENALTY);
    BIND_ENUM_CONSTANT(FLAG_GREEN);
    BIND_ENUM_CONSTANT(FLAG_ORANGE);

    BIND_ENUM_CONSTANT(MISTAKE_NONE);
    BIND_ENUM_CONSTANT(MISTAKE_PEDAL_OVERLAP);
    BIND_ENUM_CONSTANT(MISTAKE_COASTING);
    BIND_ENUM_CONSTANT(MISTAKE_ABS_ABUSE);
    BIND_ENUM_CONSTANT(MISTAKE_TRAIL_BRAKING_SMOOTHNESS);
    BIND_ENUM_CONSTANT(MISTAKE_SLOW_SHIFT);
    BIND_ENUM_CONSTANT(MISTAKE_AGGRESSIVE_DOWNSHIFT);
    BIND_ENUM_CONSTANT(MISTAKE_OVER_SLOWING);
    BIND_ENUM_CONSTANT(MISTAKE_SNAP_OVERSTEER);
    BIND_ENUM_CONSTANT(MISTAKE_THROTTLE_FLUTTER);
    BIND_ENUM_CONSTANT(MISTAKE_LOSS_OF_CONTROL);
    BIND_ENUM_CONSTANT(MISTAKE_DRIFT);
    BIND_ENUM_CONSTANT(MISTAKE_MINOR_OVERSTEER);

    ClassDB::bind_method(D_METHOD("connect_to_sim", "sim_id"), &SimTelemetryManager::connect_to_sim);
    ClassDB::bind_method(D_METHOD("detect_active_sim"), &SimTelemetryManager::detect_active_sim);
    ClassDB::bind_method(D_METHOD("disconnect_from_sim"), &SimTelemetryManager::disconnect_from_sim);
    ClassDB::bind_method(D_METHOD("is_connected_to_sim"), &SimTelemetryManager::is_connected_to_sim);
    ClassDB::bind_method(D_METHOD("get_current_sim_id"), &SimTelemetryManager::get_current_sim_id);
    ClassDB::bind_method(D_METHOD("get_sim_status"), &SimTelemetryManager::get_sim_status);

    ClassDB::bind_method(D_METHOD("start_logging", "output_file_path"), &SimTelemetryManager::start_logging);
    ClassDB::bind_method(D_METHOD("finish_logging", "output_file_path"), &SimTelemetryManager::finish_logging, DEFVAL(""));
    ClassDB::bind_method(D_METHOD("is_currently_logging"), &SimTelemetryManager::is_currently_logging);

    ClassDB::bind_method(D_METHOD("get_live_static_data"), &SimTelemetryManager::get_live_static_data);
    ClassDB::bind_method(D_METHOD("get_live_snapshot"), &SimTelemetryManager::get_live_snapshot);

    ClassDB::bind_method(D_METHOD("load_session_data", "file_path"), &SimTelemetryManager::load_session_data);
    ClassDB::bind_method(D_METHOD("get_session_metadata", "file_path"), &SimTelemetryManager::get_session_metadata);
    ClassDB::bind_method(D_METHOD("get_loaded_session_metadata"), &SimTelemetryManager::get_loaded_session_metadata);
    ClassDB::bind_method(D_METHOD("close_loaded_session"), &SimTelemetryManager::close_loaded_session);
    
    ClassDB::bind_method(D_METHOD("get_loaded_session_lap_count"), &SimTelemetryManager::get_loaded_session_lap_count);
    ClassDB::bind_method(D_METHOD("get_loaded_session_sample_interval"), &SimTelemetryManager::get_loaded_session_sample_interval);
    ClassDB::bind_method(D_METHOD("get_loaded_session_samples_per_meter"), &SimTelemetryManager::get_loaded_session_samples_per_meter);
    ClassDB::bind_method(D_METHOD("get_loaded_session_lap_data", "lap_index"), &SimTelemetryManager::get_loaded_session_lap_data);
    ClassDB::bind_method(D_METHOD("get_loaded_session_static_data"), &SimTelemetryManager::get_loaded_session_static_data);
    ClassDB::bind_method(D_METHOD("get_loaded_session_total_fuel_consumption"), &SimTelemetryManager::get_loaded_session_total_fuel_consumption);
    ClassDB::bind_method(D_METHOD("get_loaded_session_lap_fuel_consumption", "lap_index"), &SimTelemetryManager::get_loaded_session_lap_fuel_consumption);
    ClassDB::bind_method(D_METHOD("get_loaded_session_total_laps"), &SimTelemetryManager::get_loaded_session_total_laps);
    ClassDB::bind_method(D_METHOD("get_loaded_session_lap_stats", "lap_index"), &SimTelemetryManager::get_loaded_session_lap_stats);
    ClassDB::bind_method(D_METHOD("calculate_lap_time_delta", "target_file_path", "target_lap_index", "current_file_path", "current_lap_index", "reference_positions"), &SimTelemetryManager::calculate_lap_time_delta, DEFVAL(0), DEFVAL(""), DEFVAL(0), DEFVAL(PackedFloat32Array()));
    ClassDB::bind_method(D_METHOD("analyze_lap", "lap_data", "reference_lap_data"), &SimTelemetryManager::analyze_lap, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("get_array_min_max", "array"), &SimTelemetryManager::get_array_min_max);
    ClassDB::bind_method(D_METHOD("get_channel_name", "standard_name"), &SimTelemetryManager::get_channel_name);

    ClassDB::add_signal("SimTelemetryManager", MethodInfo("connection_lost"));
    ClassDB::add_signal("SimTelemetryManager", MethodInfo("session_auto_saved", PropertyInfo(Variant::STRING, "file_path")));
    ClassDB::add_signal("SimTelemetryManager", MethodInfo("compression_finished", PropertyInfo(Variant::STRING, "file_path"), PropertyInfo(Variant::BOOL, "success")));

    ClassDB::bind_method(D_METHOD("get_sample_interval"), &SimTelemetryManager::get_sample_interval);
    ClassDB::bind_method(D_METHOD("set_sample_interval", "interval"), &SimTelemetryManager::set_sample_interval);
    ClassDB::bind_method(D_METHOD("get_samples_per_meter"), &SimTelemetryManager::get_samples_per_meter);
    ClassDB::bind_method(D_METHOD("set_samples_per_meter", "spm"), &SimTelemetryManager::set_samples_per_meter);
    ClassDB::bind_method(D_METHOD("get_save_file_signature"), &SimTelemetryManager::get_save_file_signature);
    
    ClassDB::bind_method(D_METHOD("set_zstd_compression_enabled", "enabled"), &SimTelemetryManager::set_zstd_compression_enabled);
    ClassDB::bind_method(D_METHOD("is_zstd_compression_enabled"), &SimTelemetryManager::is_zstd_compression_enabled);
    ClassDB::bind_method(D_METHOD("get_compression_format", "file_path"), &SimTelemetryManager::get_compression_format);
    ClassDB::bind_method(D_METHOD("zstd_compress_file", "file_path"), &SimTelemetryManager::zstd_compress_file);
    ClassDB::bind_method(D_METHOD("zstd_compress_file_async", "file_path"), &SimTelemetryManager::zstd_compress_file_async);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sample_interval"), "set_sample_interval", "get_sample_interval");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "samples_per_meter"), "set_samples_per_meter", "get_samples_per_meter");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "zstd_compression_enabled"), "set_zstd_compression_enabled", "is_zstd_compression_enabled");
}

ISimProvider* SimTelemetryManager::_create_provider(const String& sim_id) {
    ISimProvider* provider = nullptr;
    if (sim_id == "AC") {
        provider = new ACProvider();
    } else if (sim_id == "ACC") {
        provider = new ACCProvider();
    }
    
    if (provider) {
        provider->set_auto_save_callback([this](String path) {
            this->call_deferred("emit_signal", "session_auto_saved", path);
        });
        provider->set_zstd_compression_enabled(zstd_compression_enabled);
    }
    
    return provider;
}

String SimTelemetryManager::_detect_file_signature(const String& file_path) {
    String os_path = file_path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    TelemetryFile infile;
    if (!infile.open_read(os_path)) return "";

    char sig[5] = {0};
    infile.read(sig, 4);
    
    for (int i = 0; i < 4; i++) {
        if (sig[i] < 32 || sig[i] > 126) return "";
    }
    return String(sig);
}

ISimProvider* SimTelemetryManager::_get_provider_for_signature(const String& sig) {
    if (sig == "ACTL") {
        return new ACProvider();
    }
    else if (sig == "ACCT") {
        return new ACCProvider();
    }
    return nullptr;
}

void SimTelemetryManager::_process(double delta) {
    if (active_provider) {
        bool was_connected = active_provider->is_connected();
        active_provider->update();
        if (was_connected && !active_provider->is_connected()) {
            emit_signal("connection_lost");
        }
    }
}

String SimTelemetryManager::connect_to_sim(const String& sim_id) {
    disconnect_from_sim();
    
    ISimProvider* new_prov = _create_provider(sim_id);
    if (!new_prov) return "Unsupported sim ID: " + sim_id;

    new_prov->set_sample_interval(sample_interval);
    new_prov->set_samples_per_meter(samples_per_meter);

    active_provider.reset(new_prov);
    current_sim_id = sim_id;

    return active_provider->connect_provider();
}

String SimTelemetryManager::detect_active_sim() const {
    if (ACCProvider::check_is_active()) {
        return "ACC";
    }

    if (ACProvider::check_is_active()) {
        return "AC";
    }
    
    return "";
}

void SimTelemetryManager::disconnect_from_sim() {
    if (active_provider) {
        active_provider->disconnect_provider();
        active_provider.reset();
        current_sim_id = "";
    }
}

bool SimTelemetryManager::is_connected_to_sim() const {
    if (active_provider) {
        return active_provider->is_connected();
    }
    return false;
}

String SimTelemetryManager::get_current_sim_id() const {
    return current_sim_id;
}

int SimTelemetryManager::get_sim_status() {
    if (!active_provider) return 0;
    return active_provider->get_provider_status();
}

String SimTelemetryManager::start_logging(const String& output_file_path) {
    if (!active_provider) return "No active provider.";
    return active_provider->start_capture(output_file_path);
}

String SimTelemetryManager::finish_logging(const String& output_file_path) {
    if (!active_provider) return "No active provider.";
    return active_provider->stop_capture(output_file_path);
}

bool SimTelemetryManager::is_currently_logging() const {
    if (active_provider) {
        return active_provider->is_logging_active();
    }
    return false;
}

Dictionary SimTelemetryManager::get_live_static_data() {
    if (!active_provider) return Dictionary();
    return active_provider->get_live_static_data();
}

Ref<GDLapTelemetry> SimTelemetryManager::get_live_snapshot() {
    Ref<GDLapTelemetry> snapshot;
    snapshot.instantiate();
    
    if (active_provider) {
        Dictionary dict = active_provider->get_live_snapshot();
        if (!dict.is_empty()) {
            snapshot->set_channels(dict);
        }
    }
    return snapshot;
}

String SimTelemetryManager::load_session_data(const String& file_path) {
    String os_path = file_path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    TelemetryFile infile;
    if (!infile.open_read(os_path)) return "could not open file: " + os_path;
    
    char sig[4];
    infile.read(sig, 4);
    infile.close();
    
    String signature = String::utf8(sig, 4);
    
    ISimProvider* new_prov = _get_provider_for_signature(signature);
    if (!new_prov) {
        return "Unknown or unsupported file signature: " + signature;
    }
    
    new_prov->set_sample_interval(sample_interval);
    new_prov->set_samples_per_meter(samples_per_meter);
    
    active_provider.reset(new_prov);
    return active_provider->load_session(file_path);
}

Dictionary SimTelemetryManager::get_session_metadata(const String& file_path) {
    String signature = _detect_file_signature(file_path);
    ISimProvider* temp_prov = _get_provider_for_signature(signature);
    if (!temp_prov) {
        Dictionary err; err["error"] = "Unknown or unsupported file signature"; return err;
    }
    
    Dictionary meta = temp_prov->get_session_metadata_from_file(file_path);
    delete temp_prov;
    return meta;
}

Dictionary SimTelemetryManager::get_loaded_session_metadata() {
    if (active_provider) {
        return active_provider->get_session_metadata();
    }
    return Dictionary();
}

void SimTelemetryManager::close_loaded_session() {
    if (active_provider) {
        active_provider->close_session();
    }
}

int SimTelemetryManager::get_loaded_session_lap_count() {
    if (!active_provider) return 0;
    return active_provider->get_loaded_session_lap_count();
}

double SimTelemetryManager::get_loaded_session_sample_interval() {
    if (active_provider) {
        return active_provider->get_loaded_session_sample_interval();
    }
    return 0.0;
}


void SimTelemetryManager::set_zstd_compression_enabled(bool enabled) {
    zstd_compression_enabled = enabled;
    if (active_provider) {
        active_provider->set_zstd_compression_enabled(enabled);
    }
}

bool SimTelemetryManager::is_zstd_compression_enabled() const {
    return zstd_compression_enabled;
}

String SimTelemetryManager::get_compression_format(const String& file_path) {
    String os_path = file_path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    std::ifstream check(os_path.utf8().get_data(), std::ios::binary);
    if (check.is_open()) {
        check.seekg(4, std::ios::beg);
        char sig[4];
        check.read(sig, 4);
        if (std::strncmp(sig, "ZST2", 4) == 0) return "ZSTD";
    }
    return "";
}

bool SimTelemetryManager::zstd_compress_file(const String& file_path) {
    String os_path = file_path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    // check if it's already compressed
    {
        std::ifstream check(os_path.utf8().get_data(), std::ios::binary);
        if (check.is_open()) {
            check.seekg(4, std::ios::beg);
            char sig[4];
            check.read(sig, 4);
            if (std::strncmp(sig, "ZST2", 4) == 0) return true;
        }
    }
    
    return TelemetryFile::compress_existing_file(os_path);
}

void SimTelemetryManager::zstd_compress_file_async(const String& file_path) {
    String os_path = file_path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    std::thread([this, os_path]() {
        bool res = false;
        
        // check if it's already compressed
        bool already_compressed = false;
        {
            std::ifstream check(os_path.utf8().get_data(), std::ios::binary);
            if (check.is_open()) {
                check.seekg(4, std::ios::beg);
                char sig[4];
                check.read(sig, 4);
                if (std::strncmp(sig, "ZST2", 4) == 0) already_compressed = true;
            }
        }
        
        if (already_compressed) {
            res = true;
        } else {
            res = TelemetryFile::compress_existing_file(os_path);
        }
        
        this->call_deferred("emit_signal", "compression_finished", os_path, res);
    }).detach();
}

double SimTelemetryManager::get_loaded_session_samples_per_meter() {
    if (active_provider) {
        return active_provider->get_loaded_session_samples_per_meter();
    }
    return 0.0;
}

Ref<GDLapTelemetry> SimTelemetryManager::get_loaded_session_lap_data(int lap_index) {
    if (!active_provider) return Ref<GDLapTelemetry>();
    Dictionary lap_dict = active_provider->get_lap_data(lap_index);
    if (lap_dict.is_empty()) return Ref<GDLapTelemetry>();
    
    Ref<GDLapTelemetry> lap_obj;
    lap_obj.instantiate();
    lap_obj->set_channels(lap_dict);
    return lap_obj;
}

Dictionary SimTelemetryManager::get_loaded_session_static_data() {
    if (!active_provider) return Dictionary();
    return active_provider->get_loaded_session_static_data();
}

double SimTelemetryManager::get_loaded_session_total_fuel_consumption() {
    if (!active_provider) return 0.0;
    return active_provider->get_loaded_session_total_fuel_consumption();
}

double SimTelemetryManager::get_loaded_session_lap_fuel_consumption(int lap_index) {
    if (!active_provider) return 0.0;
    return active_provider->get_loaded_session_lap_fuel_consumption(lap_index);
}

double SimTelemetryManager::get_loaded_session_total_laps() {
    if (!active_provider) return 0.0;
    return active_provider->get_loaded_session_total_laps();
}

Dictionary SimTelemetryManager::get_loaded_session_lap_stats(int lap_index) {
    if (!active_provider) return Dictionary();
    return active_provider->get_loaded_session_lap_stats(lap_index);
}

Dictionary SimTelemetryManager::calculate_lap_time_delta(const String& target_file_path, int target_lap_index, const String& current_file_path, int current_lap_index, const PackedFloat32Array& reference_positions) {
    if (!active_provider) {
        String sig = _detect_file_signature(target_file_path);
        ISimProvider* prov = _get_provider_for_signature(sig);
        if (!prov) return Dictionary();
        active_provider.reset(prov);
    }
    return active_provider->calculate_lap_time_delta(target_file_path, target_lap_index, current_file_path, current_lap_index, reference_positions);
}

String SimTelemetryManager::get_channel_name(const String& standard_name) {
    if (active_provider) {
        return active_provider->get_internal_channel_name(standard_name);
    }
    return standard_name;
}

Array SimTelemetryManager::analyze_lap(const Dictionary& lap_data, const Dictionary& reference_lap_data) {
    return DrivingAnalyzer::analyze_lap(this, lap_data, reference_lap_data);
}

Vector2 SimTelemetryManager::get_array_min_max(const Variant& arr) {
    if (arr.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
        PackedFloat32Array f_arr = arr;
        if (f_arr.is_empty()) return Vector2(0, 0);
        const float* ptr = f_arr.ptr();
        float min_val = 0.0f;
        float max_val = 0.0f;
        bool initialized = false;
        for (int i = 0; i < f_arr.size(); i++) {
            if (std::isnan(ptr[i]) || std::isinf(ptr[i])) continue;
            if (!initialized) {
                min_val = ptr[i];
                max_val = ptr[i];
                initialized = true;
            } else {
                if (ptr[i] < min_val) min_val = ptr[i];
                if (ptr[i] > max_val) max_val = ptr[i];
            }
        }
        return Vector2(min_val, max_val);
    } 
    else if (arr.get_type() == Variant::PACKED_INT32_ARRAY) {
        PackedInt32Array i_arr = arr;
        if (i_arr.is_empty()) return Vector2(0, 0);
        const int32_t* ptr = i_arr.ptr();
        float min_val = (float)ptr[0];
        float max_val = (float)ptr[0];
        for (int i = 1; i < i_arr.size(); i++) {
            if (ptr[i] < min_val) min_val = (float)ptr[i];
            if (ptr[i] > max_val) max_val = (float)ptr[i];
        }
        return Vector2(min_val, max_val);
    }
    return Vector2(0, 0);
}
