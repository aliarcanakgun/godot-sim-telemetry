#include "acc_provider.h"
#include "helper.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <cmath>
#include <algorithm>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstdio>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace godot;

ACCProvider::ACCProvider() {
    hMapPhysics = NULL;
    hMapGraphic = NULL;
    hMapStatic = NULL;
    dataPhysics = nullptr;
    dataGraphic = nullptr;
    dataStatic = nullptr;
}

ACCProvider::~ACCProvider() {
    is_logging = false;
    if (logging_thread.joinable()) {
        logging_thread.join();
    }
    disconnect_provider();
}

bool ACCProvider::check_is_active() {
#ifdef _WIN32
    HANDLE hMapStatic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (hMapStatic) {
        void* map_data = MapViewOfFile(hMapStatic, FILE_MAP_READ, 0, 0, 60);
        if (map_data) {
            String smVersion = wchar_to_gdstring((const wchar_t*)map_data, 15);
            UnmapViewOfFile(map_data);
            CloseHandle(hMapStatic);
            return !smVersion.begins_with("1.7"); // can be improved
        }
        CloseHandle(hMapStatic);
    }
#endif
    return false;
}

String ACCProvider::connect_provider() {
    is_connected_flag = false;

    if (dataPhysics) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; }
    if (dataGraphic) { UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; }
    if (hMapPhysics) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; }
    if (hMapGraphic) { CloseHandle(hMapGraphic); hMapGraphic = nullptr; }

    hMapPhysics = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
    if (hMapPhysics == NULL) return "Physics Map Error";

    dataPhysics = (ACC_SPagePhysics*)MapViewOfFile(hMapPhysics, FILE_MAP_READ, 0, 0, sizeof(ACC_SPagePhysics));
    if (dataPhysics == nullptr) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; return "Physics MapViewOfFile failed"; }

    hMapGraphic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
    if (hMapGraphic == NULL) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr; return "Graphic Map Error"; }
    
    dataGraphic = (ACC_SPageGraphic*)MapViewOfFile(hMapGraphic, FILE_MAP_READ, 0, 0, sizeof(ACC_SPageGraphic));
    if (dataGraphic == nullptr) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        CloseHandle(hMapGraphic); hMapGraphic = nullptr; return "Graphic MapViewOfFile failed";
    }

    hMapStatic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (hMapStatic == NULL) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; CloseHandle(hMapGraphic); hMapGraphic = nullptr; return "Static Map Error";
    }
    
    dataStatic = (ACC_SPageStatic*)MapViewOfFile(hMapStatic, FILE_MAP_READ, 0, 0, sizeof(ACC_SPageStatic));
    if (dataStatic == nullptr) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; CloseHandle(hMapGraphic); hMapGraphic = nullptr;
        CloseHandle(hMapStatic); hMapStatic = nullptr; return "Static MapViewOfFile failed";
    }

    is_connected_flag = true;
    return "";
}

void ACCProvider::disconnect_provider() {
    stop_capture();
    is_connected_flag = false;
    if (dataPhysics) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; }
    if (dataGraphic) { UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; }
    if (dataStatic) { UnmapViewOfFile(dataStatic); dataStatic = nullptr; }
    if (hMapPhysics) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; }
    if (hMapGraphic) { CloseHandle(hMapGraphic); hMapGraphic = nullptr; }
    if (hMapStatic) { CloseHandle(hMapStatic); hMapStatic = nullptr; }
}

void ACCProvider::update() {
    if (!is_connected_flag) return;
    if (game_disconnected.load()) {
        game_disconnected.store(false);
        if (is_connected_flag) {
            stop_capture(session_output_file_path);
            is_connected_flag = false;
        }
        return;
    }
    if (!dataPhysics || !dataGraphic) {
        if (is_connected_flag) {
            is_connected_flag = false;
            stop_capture(session_output_file_path);
        }
    }
}

void ACCProvider::set_auto_save_callback(std::function<void(godot::String)> callback) {
    auto_save_callback = callback;
}

String ACCProvider::start_capture(const String& output_file_path) {
    if (output_file_path.is_empty()) return "Output file path is empty.";
    if (is_logging) return "Already logging.";
    session_output_file_path = output_file_path;

    session_type_counts.clear();
    sessions_data.clear();
    last_lap_count = 0;
    last_physics_packet_id = -1;
    last_i_current_time = 0;
    last_recorded_meter = -1.0;
    game_disconnected.store(false);
    is_logging = true;

    logging_thread = std::thread(&ACCProvider::logging_loop, this);
    return "";
}

String ACCProvider::stop_capture(const String& output_file_path) {
    if (!is_connected_flag) return "ACC is not connected.";
    if (!is_logging) return "Telemetry is not working.";

    is_logging = false;
    if (logging_thread.joinable()) {
        logging_thread.join();
    }

    std::lock_guard<std::mutex> lock(data_mutex);

    if (sessions_data.empty()) return "No valid telemetry data was recorded.";
    if (!dataStatic || !dataGraphic) return "Data pointer is null";

    String output = output_file_path;
    if (output.is_empty()) output = session_output_file_path;

    int current_count = ++session_type_counts[dataGraphic->session];
    String session_suffix = _get_session_suffix(dataGraphic->session);
    if (current_count > 1) {
        session_suffix += "_" + String::num_int64(current_count);
    }

    String extension = output.get_extension();
    String base_path = output.get_basename();
    String final_path = base_path + session_suffix + "." + extension;

    std::vector<ACC_LapDataChannels> data_to_save = std::move(sessions_data);
    sessions_data.clear();
    
    ACC_SPageStatic static_copy = *dataStatic;

    double s_interval = sample_interval;
    double s_spm = samples_per_meter;
    
    std::thread([this, data_to_save = std::move(data_to_save), static_copy, s_interval, s_spm, final_path]() mutable {
        this->_flush_sessions_to_disk(std::move(data_to_save), static_copy, s_interval, s_spm, final_path);
    }).detach();

    return final_path;
}

String ACCProvider::_get_session_suffix(int session_enum) {
    switch (session_enum) {
        case ACC_UNKNOWN: return "";
        case ACC_PRACTICE: return ".prac";
        case ACC_QUALIFY: return ".quali";
        case ACC_RACE: return ".race";
        case ACC_HOTLAP: return ".hl";
        case ACC_TIMEATTACK: return ".ta";
        case ACC_DRIFT: return ".drift";
        case ACC_DRAG: return ".drag";
        case ACC_HOTSTINT: return ".hs";
        case ACC_HOTSTINTSUPERPOLE: return ".sp";
        default: return "";
    }
}

void ACCProvider::_flush_sessions_to_disk(std::vector<ACC_LapDataChannels> data_to_save, ACC_SPageStatic static_data_copy, double save_interval, double save_spm, String path) {
    // remove empty/junk laps
    // NOTE: some checks will yield incorrect results
    //       if the last lap is removed.

    // for (auto it = data_to_save.begin(); it != data_to_save.end(); ) {
    //     if (it->speedKmh.empty() || it->iCurrentTime.empty() || it->timestamp.size() < 10) {
    //         it = data_to_save.erase(it);
    //     } else {
    //         ++it;
    //     }
    // }
    
    if (data_to_save.empty()) return; // nothing to save

    std::vector<float> boundaries = get_acc_sectors(wchar_to_gdstring(static_data_copy.track, 33).to_lower());
    for (auto& lap : data_to_save) {
        if (lap.speedKmh.empty()) continue;
        int current_sector = 0;
        
        // initialize correct sector for the starting position of the lap
        if (!lap.normalizedCarPosition.empty()) {
            float start_pos = lap.normalizedCarPosition[0];
            while (current_sector < boundaries.size() && start_pos >= boundaries[current_sector]) {
                current_sector++;
            }
        }

        for (size_t j = 0; j < lap.normalizedCarPosition.size(); j++) {
            float pos = lap.normalizedCarPosition[j];
            while (current_sector < boundaries.size() && pos >= boundaries[current_sector]) {
                current_sector++;
            }
            if (j < lap.currentSectorIndex.size()) {
                lap.currentSectorIndex[j] = current_sector;
            }
        }
    }
    
    String os_path = path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }

    CharString cs = os_path.utf8();
    std::string filepath(cs.get_data(), cs.length());

    TelemetryFile outfile;
    if (!outfile.open_write()) return;

    CharString utf8_signature = save_file_signature.utf8();
    outfile.write(utf8_signature.get_data(), utf8_signature.length());
    if (outfile.fail()) { outfile.close(); return; }

    int64_t session_timestamp = Time::get_singleton()->get_unix_time_from_system();
    outfile.write(&session_timestamp, sizeof(int64_t));
    if (outfile.fail()) { outfile.close(); return; }

    outfile.write(&static_data_copy, sizeof(ACC_SPageStatic));
    if (outfile.fail()) { outfile.close(); return; }

    outfile.write(&save_interval, sizeof(double));
    outfile.write(&save_spm, sizeof(double));

    uint64_t total_laps = data_to_save.size();
    outfile.write(&total_laps, sizeof(total_laps));

    uint64_t offsets_pos = outfile.tellp();
    std::vector<uint64_t> lap_offsets(total_laps, 0);
    outfile.write(lap_offsets.data(), total_laps * sizeof(uint64_t));

    for (uint64_t idx = 0; idx < data_to_save.size(); ++idx) {
        lap_offsets[idx] = outfile.tellp();
        data_to_save[idx].write_to_stream(outfile);
    }

    outfile.seekp(offsets_pos);
    outfile.write(lap_offsets.data(), total_laps * sizeof(uint64_t));

    outfile.close_and_save(os_path, compression_method == COMPRESSION_ZSTD);
}

bool ACCProvider::is_logging_active() const {
    return is_logging.load();
}

int ACCProvider::get_provider_status() const {
    if (!dataGraphic) return 0;
    return dataGraphic->status;
}

void ACCProvider::apply_math_conversions_in_place(ACC_LapDataChannels& lap) {
    // transform vectors for godot dict arrays
    
    // pct
    for (size_t i=0; i<lap.gas.size(); ++i) lap.gas[i] *= 100.0f;
    for (size_t i=0; i<lap.brake.size(); ++i) lap.brake[i] *= 100.0f;
    for (size_t i=0; i<lap.steerAngle.size(); ++i) lap.steerAngle[i] *= 100.0f;
    for (size_t i=0; i<lap.tc.size(); ++i) lap.tc[i] *= 100.0f;
    for (size_t i=0; i<lap.abs.size(); ++i) lap.abs[i] *= 100.0f;
    for (size_t i=0; i<lap.kersCharge.size(); ++i) lap.kersCharge[i] *= 100.0f;
    for (size_t i=0; i<lap.kersInput.size(); ++i) lap.kersInput[i] *= 100.0f;
    for (size_t i=0; i<lap.brakeBias.size(); ++i) lap.brakeBias[i] *= 100.0f;
    for (size_t i=0; i<lap.clutch.size(); ++i) lap.clutch[i] = (1.0f - lap.clutch[i]) * 100.0f;

    // offset
    for (size_t i=0; i<lap.gear.size(); ++i) lap.gear[i] -= 1;
    
    // rad to deg
    auto to_deg = [](std::vector<float>& v) { for (size_t i=0; i<v.size(); ++i) v[i] *= 57.29578f; };
    to_deg(lap.camberRAD_fl); to_deg(lap.camberRAD_fr); to_deg(lap.camberRAD_rl); to_deg(lap.camberRAD_rr);
    to_deg(lap.slipAngle_FL); to_deg(lap.slipAngle_FR); to_deg(lap.slipAngle_RL); to_deg(lap.slipAngle_RR);
    to_deg(lap.heading); to_deg(lap.pitch); to_deg(lap.roll);
    to_deg(lap.localAngularVel_x); to_deg(lap.localAngularVel_y); to_deg(lap.localAngularVel_z);
    
    // m to mm
    auto to_mm = [](std::vector<float>& v) { for (size_t i=0; i<v.size(); ++i) v[i] *= 1000.0f; };
    to_mm(lap.cgHeight); to_mm(lap.rideHeight_f); to_mm(lap.rideHeight_r);
    to_mm(lap.suspensionTravel_fl); to_mm(lap.suspensionTravel_fr); to_mm(lap.suspensionTravel_rl); to_mm(lap.suspensionTravel_rr);
    
    // rad/s to rpm
    auto to_rpm = [](std::vector<float>& v) { for (size_t i=0; i<v.size(); ++i) v[i] *= 9.549297f; };
    to_rpm(lap.wheelAngularSpeed_fl); to_rpm(lap.wheelAngularSpeed_fr); to_rpm(lap.wheelAngularSpeed_rl); to_rpm(lap.wheelAngularSpeed_rr);
}

Dictionary ACCProvider::_lap_to_dict(const ACC_LapDataChannels& c) {
    Dictionary d;
    // map vectors to godot packed arrays
    d["timestamp"] = to_float_array(c.timestamp);
    d["packetId_graphic"] = to_int_array(c.packetId_graphic);
    d["iCurrentTime"] = to_int_array(c.iCurrentTime);
    d["iLastTime"] = to_int_array(c.iLastTime);
    d["iBestTime"] = to_int_array(c.iBestTime);
    d["normalizedCarPosition"] = to_float_array(c.normalizedCarPosition);
    d["distanceTraveled"] = to_float_array(c.distanceTraveled);
    d["replayTimeMultiplier"] = to_float_array(c.replayTimeMultiplier);
    d["numberOfLaps"] = to_int_array(c.numberOfLaps);
    d["completedLaps"] = to_int_array(c.completedLaps);
    
    d["currentTime"] = to_string_array(c.currentTime);
    d["lastTime"] = to_string_array(c.lastTime);
    d["bestTime"] = to_string_array(c.bestTime);
    d["split"] = to_string_array(c.split);
    d["tyreCompound"] = to_string_array(c.tyreCompound);

    d["packetId_physics"] = to_int_array(c.packetId_physics);
    d["gas"] = to_float_array(c.gas); // already converted
    d["brake"] = to_float_array(c.brake); // already converted
    d["fuel"] = to_float_array(c.fuel);
    d["gear"] = to_int_array(c.gear); // already converted
    d["rpms"] = to_int_array(c.rpms);
    d["steerAngle"] = to_float_array(c.steerAngle); // already converted
    
    int derivative_smoothing_window = std::max(3, (int)(5.0 * samples_per_meter));
    if (derivative_smoothing_window % 2 == 0) derivative_smoothing_window += 1;
    d["steerVelo"] = calc_derivative(c.steerAngle, c.iCurrentTime, derivative_smoothing_window, 1.0f);
    
    d["speedKmh"] = to_float_array(c.speedKmh);
    d["isAIControlled"] = to_int_array(c.isAIControlled);
    
    // combined gas/brake
    PackedFloat32Array combined_pedals;
    combined_pedals.resize(c.gas.size());
    float* combined_ptr = combined_pedals.ptrw();
    for (size_t i = 0; i < c.gas.size(); i++) {
        combined_ptr[i] = c.gas[i] - c.brake[i];
    }
    d["gasBrakeCombined"] = combined_pedals;

    d["velocity_x"] = to_float_array(c.velocity_x);
    d["velocity_y"] = to_float_array(c.velocity_y);
    d["velocity_z"] = to_float_array(c.velocity_z);
    d["accG_x"] = to_float_array(c.accG_x);
    d["accG_y"] = to_float_array(c.accG_y);
    d["accG_z"] = to_float_array(c.accG_z);

    d["wheelSlip_fl"] = to_float_array(c.wheelSlip_fl); d["wheelSlip_fr"] = to_float_array(c.wheelSlip_fr); d["wheelSlip_rl"] = to_float_array(c.wheelSlip_rl); d["wheelSlip_rr"] = to_float_array(c.wheelSlip_rr);
    d["wheelLoad_fl"] = to_float_array(c.wheelLoad_fl); d["wheelLoad_fr"] = to_float_array(c.wheelLoad_fr); d["wheelLoad_rl"] = to_float_array(c.wheelLoad_rl); d["wheelLoad_rr"] = to_float_array(c.wheelLoad_rr);
    d["wheelsPressure_fl"] = to_float_array(c.wheelsPressure_fl); d["wheelsPressure_fr"] = to_float_array(c.wheelsPressure_fr); d["wheelsPressure_rl"] = to_float_array(c.wheelsPressure_rl); d["wheelsPressure_rr"] = to_float_array(c.wheelsPressure_rr);
    
    d["wheelAngularSpeed_fl"] = to_float_array(c.wheelAngularSpeed_fl); d["wheelAngularSpeed_fr"] = to_float_array(c.wheelAngularSpeed_fr); d["wheelAngularSpeed_rl"] = to_float_array(c.wheelAngularSpeed_rl); d["wheelAngularSpeed_rr"] = to_float_array(c.wheelAngularSpeed_rr);
    d["tyreWear_fl"] = to_float_array(c.tyreWear_fl); d["tyreWear_fr"] = to_float_array(c.tyreWear_fr); d["tyreWear_rl"] = to_float_array(c.tyreWear_rl); d["tyreWear_rr"] = to_float_array(c.tyreWear_rr);
    d["tyreDirtyLevel_fl"] = to_float_array(c.tyreDirtyLevel_fl); d["tyreDirtyLevel_fr"] = to_float_array(c.tyreDirtyLevel_fr); d["tyreDirtyLevel_rl"] = to_float_array(c.tyreDirtyLevel_rl); d["tyreDirtyLevel_rr"] = to_float_array(c.tyreDirtyLevel_rr);
    d["tyreCoreTemperature_fl"] = to_float_array(c.tyreCoreTemperature_fl); d["tyreCoreTemperature_fr"] = to_float_array(c.tyreCoreTemperature_fr); d["tyreCoreTemperature_rl"] = to_float_array(c.tyreCoreTemperature_rl); d["tyreCoreTemperature_rr"] = to_float_array(c.tyreCoreTemperature_rr);
    
    d["camberDEG_fl"] = to_float_array(c.camberRAD_fl); d["camberDEG_fr"] = to_float_array(c.camberRAD_fr); d["camberDEG_rl"] = to_float_array(c.camberRAD_rl); d["camberDEG_rr"] = to_float_array(c.camberRAD_rr);
    d["suspensionTravel_fl"] = to_float_array(c.suspensionTravel_fl); d["suspensionTravel_fr"] = to_float_array(c.suspensionTravel_fr); d["suspensionTravel_rl"] = to_float_array(c.suspensionTravel_rl); d["suspensionTravel_rr"] = to_float_array(c.suspensionTravel_rr);
    
    d["damperVelocity_fl"] = calc_derivative(c.suspensionTravel_fl, c.iCurrentTime, derivative_smoothing_window, 1.0f);
    d["damperVelocity_fr"] = calc_derivative(c.suspensionTravel_fr, c.iCurrentTime, derivative_smoothing_window, 1.0f);
    d["damperVelocity_rl"] = calc_derivative(c.suspensionTravel_rl, c.iCurrentTime, derivative_smoothing_window, 1.0f);
    d["damperVelocity_rr"] = calc_derivative(c.suspensionTravel_rr, c.iCurrentTime, derivative_smoothing_window, 1.0f);

    d["brakeTemp_fl"] = to_float_array(c.brakeTemp_fl); d["brakeTemp_fr"] = to_float_array(c.brakeTemp_fr); d["brakeTemp_rl"] = to_float_array(c.brakeTemp_rl); d["brakeTemp_rr"] = to_float_array(c.brakeTemp_rr);
    d["tyreTempI_fl"] = to_float_array(c.tyreTempI_fl); d["tyreTempI_fr"] = to_float_array(c.tyreTempI_fr); d["tyreTempI_rl"] = to_float_array(c.tyreTempI_rl); d["tyreTempI_rr"] = to_float_array(c.tyreTempI_rr);
    d["tyreTempM_fl"] = to_float_array(c.tyreTempM_fl); d["tyreTempM_fr"] = to_float_array(c.tyreTempM_fr); d["tyreTempM_rl"] = to_float_array(c.tyreTempM_rl); d["tyreTempM_rr"] = to_float_array(c.tyreTempM_rr);
    d["tyreTempO_fl"] = to_float_array(c.tyreTempO_fl); d["tyreTempO_fr"] = to_float_array(c.tyreTempO_fr); d["tyreTempO_rl"] = to_float_array(c.tyreTempO_rl); d["tyreTempO_rr"] = to_float_array(c.tyreTempO_rr);
    
    d["tyreContactPoint_fl_x"] = to_float_array(c.tyreContactPoint_fl_x); d["tyreContactPoint_fl_y"] = to_float_array(c.tyreContactPoint_fl_y); d["tyreContactPoint_fl_z"] = to_float_array(c.tyreContactPoint_fl_z);
    d["tyreContactPoint_fr_x"] = to_float_array(c.tyreContactPoint_fr_x); d["tyreContactPoint_fr_y"] = to_float_array(c.tyreContactPoint_fr_y); d["tyreContactPoint_fr_z"] = to_float_array(c.tyreContactPoint_fr_z);
    d["tyreContactPoint_rl_x"] = to_float_array(c.tyreContactPoint_rl_x); d["tyreContactPoint_rl_y"] = to_float_array(c.tyreContactPoint_rl_y); d["tyreContactPoint_rl_z"] = to_float_array(c.tyreContactPoint_rl_z);
    d["tyreContactPoint_rr_x"] = to_float_array(c.tyreContactPoint_rr_x); d["tyreContactPoint_rr_y"] = to_float_array(c.tyreContactPoint_rr_y); d["tyreContactPoint_rr_z"] = to_float_array(c.tyreContactPoint_rr_z);

    d["tyreContactNormal_fl_x"] = to_float_array(c.tyreContactNormal_fl_x); d["tyreContactNormal_fl_y"] = to_float_array(c.tyreContactNormal_fl_y); d["tyreContactNormal_fl_z"] = to_float_array(c.tyreContactNormal_fl_z);
    d["tyreContactNormal_fr_x"] = to_float_array(c.tyreContactNormal_fr_x); d["tyreContactNormal_fr_y"] = to_float_array(c.tyreContactNormal_fr_y); d["tyreContactNormal_fr_z"] = to_float_array(c.tyreContactNormal_fr_z);
    d["tyreContactNormal_rl_x"] = to_float_array(c.tyreContactNormal_rl_x); d["tyreContactNormal_rl_y"] = to_float_array(c.tyreContactNormal_rl_y); d["tyreContactNormal_rl_z"] = to_float_array(c.tyreContactNormal_rl_z);
    d["tyreContactNormal_rr_x"] = to_float_array(c.tyreContactNormal_rr_x); d["tyreContactNormal_rr_y"] = to_float_array(c.tyreContactNormal_rr_y); d["tyreContactNormal_rr_z"] = to_float_array(c.tyreContactNormal_rr_z);

    d["tyreContactHeading_fl_x"] = to_float_array(c.tyreContactHeading_fl_x); d["tyreContactHeading_fl_y"] = to_float_array(c.tyreContactHeading_fl_y); d["tyreContactHeading_fl_z"] = to_float_array(c.tyreContactHeading_fl_z);
    d["tyreContactHeading_fr_x"] = to_float_array(c.tyreContactHeading_fr_x); d["tyreContactHeading_fr_y"] = to_float_array(c.tyreContactHeading_fr_y); d["tyreContactHeading_fr_z"] = to_float_array(c.tyreContactHeading_fr_z);
    d["tyreContactHeading_rl_x"] = to_float_array(c.tyreContactHeading_rl_x); d["tyreContactHeading_rl_y"] = to_float_array(c.tyreContactHeading_rl_y); d["tyreContactHeading_rl_z"] = to_float_array(c.tyreContactHeading_rl_z);
    d["tyreContactHeading_rr_x"] = to_float_array(c.tyreContactHeading_rr_x); d["tyreContactHeading_rr_y"] = to_float_array(c.tyreContactHeading_rr_y); d["tyreContactHeading_rr_z"] = to_float_array(c.tyreContactHeading_rr_z);

    d["drs"] = to_float_array(c.drs);
    d["tc"] = to_float_array(c.tc);
    d["heading"] = to_float_array(c.heading);
    d["pitch"] = to_float_array(c.pitch);
    d["roll"] = to_float_array(c.roll);
    d["cgHeight"] = to_float_array(c.cgHeight);
    d["pitLimiterOn"] = to_int_array(c.pitLimiterOn);
    d["abs"] = to_float_array(c.abs);
    d["kersCharge"] = to_float_array(c.kersCharge);
    d["kersInput"] = to_float_array(c.kersInput);
    d["autoShifterOn"] = to_int_array(c.autoShifterOn);
    d["rideHeight_f"] = to_float_array(c.rideHeight_f);
    d["rideHeight_r"] = to_float_array(c.rideHeight_r);
    d["turboBoost"] = to_float_array(c.turboBoost);
    d["ballast"] = to_float_array(c.ballast);
    d["airDensity"] = to_float_array(c.airDensity);
    d["airTemp"] = to_float_array(c.airTemp);
    d["roadTemp"] = to_float_array(c.roadTemp);
    d["localAngularVel_x"] = to_float_array(c.localAngularVel_x);
    d["localAngularVel_y"] = to_float_array(c.localAngularVel_y);
    d["localAngularVel_z"] = to_float_array(c.localAngularVel_z);
    d["finalFF"] = to_float_array(c.finalFF);
    d["performanceMeter"] = to_float_array(c.performanceMeter);
    d["engineBrake"] = to_int_array(c.engineBrake);
    d["ersRecoveryLevel"] = to_int_array(c.ersRecoveryLevel);
    d["ersPowerLevel"] = to_int_array(c.ersPowerLevel);
    d["ersHeatCharging"] = to_int_array(c.ersHeatCharging);
    d["ersIsCharging"] = to_int_array(c.ersIsCharging);
    d["kersCurrentKJ"] = to_float_array(c.kersCurrentKJ);
    d["drsAvailable"] = to_int_array(c.drsAvailable);
    d["drsEnabled"] = to_int_array(c.drsEnabled);
    d["clutch"] = to_float_array(c.clutch);
    d["brakeBias_raw"] = to_float_array(c.brakeBias);

    String car_model = wchar_to_gdstring(loaded_session_static_data.carModel, 33);
    d["brakeBias"] = to_float_array_offset(c.brakeBias, get_acc_bbias_offset(car_model));
    
    d["localVelocity_x"] = to_float_array(c.localVelocity_x);
    d["localVelocity_y"] = to_float_array(c.localVelocity_y);
    d["localVelocity_z"] = to_float_array(c.localVelocity_z);
    
    d["carDamage_0"] = to_float_array(c.carDamage_0);
    d["carDamage_1"] = to_float_array(c.carDamage_1);
    d["carDamage_2"] = to_float_array(c.carDamage_2);
    d["carDamage_3"] = to_float_array(c.carDamage_3);
    d["carDamage_4"] = to_float_array(c.carDamage_4);
    d["numberOfTyresOut"] = to_int_array(c.numberOfTyresOut);

    d["status"] = to_int_array(c.status);
    d["session"] = to_int_array(c.session);
    d["position"] = to_int_array(c.position);
    d["sessionTimeLeft"] = to_float_array(c.sessionTimeLeft);
    d["isInPit"] = to_int_array(c.isInPit);
    d["currentSectorIndex"] = to_int_array(c.currentSectorIndex);
    d["lastSectorTime"] = to_int_array(c.lastSectorTime);
    d["carCoordinates_x"] = to_float_array(c.carCoordinates_x);
    d["carCoordinates_y"] = to_float_array(c.carCoordinates_y);
    d["carCoordinates_z"] = to_float_array(c.carCoordinates_z);
    d["carCoordinates"] = to_vector3_array(c.carCoordinates);
    d["penaltyTime"] = to_float_array(c.penaltyTime);
    d["flag"] = to_int_array(c.flag);
    d["idealLineOn"] = to_int_array(c.idealLineOn);
    d["isInPitLane"] = to_int_array(c.isInPitLane);
    d["surfaceGrip"] = to_float_array(c.surfaceGrip);
    d["mandatoryPitDone"] = to_int_array(c.mandatoryPitDone);
    d["windSpeed"] = to_float_array(c.windSpeed);
    d["windDirection"] = to_float_array(c.windDirection);

    // acc physics
    d["mz_FL"] = to_float_array(c.mz_FL); d["mz_FR"] = to_float_array(c.mz_FR); d["mz_RL"] = to_float_array(c.mz_RL); d["mz_RR"] = to_float_array(c.mz_RR);
    d["fx_FL"] = to_float_array(c.fx_FL); d["fx_FR"] = to_float_array(c.fx_FR); d["fx_RL"] = to_float_array(c.fx_RL); d["fx_RR"] = to_float_array(c.fx_RR);
    d["fy_FL"] = to_float_array(c.fy_FL); d["fy_FR"] = to_float_array(c.fy_FR); d["fy_RL"] = to_float_array(c.fy_RL); d["fy_RR"] = to_float_array(c.fy_RR);
    d["slipRatio_FL"] = to_float_array(c.slipRatio_FL); d["slipRatio_FR"] = to_float_array(c.slipRatio_FR); d["slipRatio_RL"] = to_float_array(c.slipRatio_RL); d["slipRatio_RR"] = to_float_array(c.slipRatio_RR);
    d["slipAngle_FL"] = to_float_array(c.slipAngle_FL); d["slipAngle_FR"] = to_float_array(c.slipAngle_FR); d["slipAngle_RL"] = to_float_array(c.slipAngle_RL); d["slipAngle_RR"] = to_float_array(c.slipAngle_RR);
    
    d["tcinAction"] = to_int_array(c.tcinAction);
    d["absInAction"] = to_int_array(c.absInAction);
    
    d["suspensionDamage_FL"] = to_float_array(c.suspensionDamage_FL); d["suspensionDamage_FR"] = to_float_array(c.suspensionDamage_FR); d["suspensionDamage_RL"] = to_float_array(c.suspensionDamage_RL); d["suspensionDamage_RR"] = to_float_array(c.suspensionDamage_RR);
    d["tyreTemp_FL"] = to_float_array(c.tyreTemp_FL); d["tyreTemp_FR"] = to_float_array(c.tyreTemp_FR); d["tyreTemp_RL"] = to_float_array(c.tyreTemp_RL); d["tyreTemp_RR"] = to_float_array(c.tyreTemp_RR);
    d["waterTemp"] = to_float_array(c.waterTemp);
    
    float front_bpressure_mult = get_acc_bpressure_multiplier(car_model, true) * 10.0f; // multiply by 10 to
    float rear_bpressure_mult = get_acc_bpressure_multiplier(car_model, false) * 10.0f; // convert it to bar
    d["brakePressure_FL"] = to_float_array_multiplier(c.brakePressure_FL, front_bpressure_mult);
    d["brakePressure_FR"] = to_float_array_multiplier(c.brakePressure_FR, front_bpressure_mult);
    d["brakePressure_RL"] = to_float_array_multiplier(c.brakePressure_RL, rear_bpressure_mult);
    d["brakePressure_RR"] = to_float_array_multiplier(c.brakePressure_RR, rear_bpressure_mult);

    d["padLife_FL"] = to_float_array(c.padLife_FL); d["padLife_FR"] = to_float_array(c.padLife_FR); d["padLife_RL"] = to_float_array(c.padLife_RL); d["padLife_RR"] = to_float_array(c.padLife_RR);
    d["discLife_FL"] = to_float_array(c.discLife_FL); d["discLife_FR"] = to_float_array(c.discLife_FR); d["discLife_RL"] = to_float_array(c.discLife_RL); d["discLife_RR"] = to_float_array(c.discLife_RR);
    
    d["kerbVibration"] = to_float_array(c.kerbVibration);
    d["slipVibrations"] = to_float_array(c.slipVibrations);
    d["gVibrations"] = to_float_array(c.gVibrations);
    d["absVibrations"] = to_float_array(c.absVibrations);

    // acc graphic
    d["packetId"] = to_int_array(c.packetId_graphic);
    d["activeCars"] = to_int_array(c.activeCars);
    d["playerCarID"] = to_int_array(c.playerCarID);
    d["penalty"] = to_int_array(c.penalty);
    
    d["TC"] = to_int_array(c.TC);
    d["TCCUT"] = to_int_array(c.TCCUT);
    d["EngineMap"] = to_int_array(c.EngineMap);
    d["ABS"] = to_int_array(c.ABS);
    d["exhaustTemperature"] = to_float_array(c.exhaustTemperature);
    d["isSetupMenuVisible"] = to_int_array(c.isSetupMenuVisible);
    d["mainDisplayIndex"] = to_int_array(c.mainDisplayIndex);
    d["secondaryDisplyIndex"] = to_int_array(c.secondaryDisplyIndex);
    d["fuelXLap"] = to_float_array(c.fuelXLap);
    d["rainLights"] = to_int_array(c.rainLights);
    d["flashingLights"] = to_int_array(c.flashingLights);
    d["lightsStage"] = to_int_array(c.lightsStage);
    d["wiperLV"] = to_int_array(c.wiperLV);
    d["driverStintTotalTimeLeft"] = to_int_array(c.driverStintTotalTimeLeft);
    d["driverStintTimeLeft"] = to_int_array(c.driverStintTimeLeft);
    d["rainTyres"] = to_int_array(c.rainTyres);
    d["sessionIndex"] = to_int_array(c.sessionIndex);
    d["usedFuel"] = to_float_array(c.usedFuel);
    d["deltaLapTime"] = to_string_array(c.deltaLapTime);
    d["iDeltaLapTime"] = to_int_array(c.iDeltaLapTime);
    d["estimatedLapTime"] = to_string_array(c.estimatedLapTime);
    d["iEstimatedLapTime"] = to_int_array(c.iEstimatedLapTime);
    d["isDeltaPositive"] = to_int_array(c.isDeltaPositive);
    d["iSplit"] = to_int_array(c.iSplit);
    d["isValidLap"] = to_int_array(c.isValidLap);
    d["fuelEstimatedLaps"] = to_float_array(c.fuelEstimatedLaps);
    d["trackStatus"] = to_string_array(c.trackStatus);
    d["missingMandatoryPits"] = to_int_array(c.missingMandatoryPits);
    d["Clock"] = to_float_array(c.Clock);
    d["directionLightsLeft"] = to_int_array(c.directionLightsLeft);
    d["directionLightsRight"] = to_int_array(c.directionLightsRight);
    d["GlobalYellow"] = to_int_array(c.GlobalYellow);
    d["GlobalYellow1"] = to_int_array(c.GlobalYellow1);
    d["GlobalYellow2"] = to_int_array(c.GlobalYellow2);
    d["GlobalYellow3"] = to_int_array(c.GlobalYellow3);
    d["GlobalWhite"] = to_int_array(c.GlobalWhite);
    d["GlobalGreen"] = to_int_array(c.GlobalGreen);
    d["GlobalChequered"] = to_int_array(c.GlobalChequered);
    d["GlobalRed"] = to_int_array(c.GlobalRed);
    d["mfdTyreSet"] = to_int_array(c.mfdTyreSet);
    d["mfdFuelToAdd"] = to_float_array(c.mfdFuelToAdd);
    d["mfdTyrePressureLF"] = to_float_array(c.mfdTyrePressureLF);
    d["mfdTyrePressureRF"] = to_float_array(c.mfdTyrePressureRF);
    d["mfdTyrePressureLR"] = to_float_array(c.mfdTyrePressureLR);
    d["mfdTyrePressureRR"] = to_float_array(c.mfdTyrePressureRR);
    d["trackGripStatus"] = to_int_array(c.trackGripStatus);
    d["rainIntensity"] = to_int_array(c.rainIntensity);
    d["rainIntensityIn10min"] = to_int_array(c.rainIntensityIn10min);
    d["rainIntensityIn30min"] = to_int_array(c.rainIntensityIn30min);
    d["currentTyreSet"] = to_int_array(c.currentTyreSet);
    d["strategyTyreSet"] = to_int_array(c.strategyTyreSet);
    d["gapAhead"] = to_int_array(c.gapAhead);
    d["gapBehind"] = to_int_array(c.gapBehind);

    return d;
}

Dictionary ACCProvider::get_live_snapshot() {
    if (!is_connected_flag || !dataPhysics || !dataGraphic || sessions_data.empty()) {
        return Dictionary();
    }
    std::lock_guard<std::mutex> lock(data_mutex);
    
    // copy last lap to prevent modification
    ACC_LapDataChannels lap_copy = sessions_data.back();
    apply_math_conversions_in_place(lap_copy);
    return _lap_to_dict(lap_copy);
}

Dictionary ACCProvider::get_lap_data(int lap_index) {
    if (lap_index < 0 || lap_index >= loaded_session_data.size()) {
        return Dictionary();
    }
    ACC_LapDataChannels lap_copy = loaded_session_data[lap_index];
    apply_math_conversions_in_place(lap_copy);
    return _lap_to_dict(lap_copy);
}

Dictionary ACCProvider::get_session_metadata_from_file(const String& file_path) {
    loaded_session_data.clear();
    loaded_session_lap_offsets.clear();

    TelemetryFile infile;
    uint64_t count = 0;
    int64_t dummy_ts = 0;
    String err = _open_session_file(file_path, infile, loaded_session_static_data, loaded_session_sample_interval, loaded_session_samples_per_meter, count, loaded_session_lap_offsets, dummy_ts);
    if (!err.is_empty()) return Dictionary();

    loaded_session_lap_count = count;

    for (int i = 0; i < loaded_session_lap_count; ++i) {
        ACC_LapDataChannels lap_data;
        infile.seekg(loaded_session_lap_offsets[i]);
        lap_data.read_from_stream(infile);
        if (infile.fail()) {
            return Dictionary();
        }
        
        if (lap_data.speedKmh.empty()) continue;

        if (!lap_data.normalizedCarPosition.empty()) {
            float base_offset = 0.0f;
            float prev_pos = lap_data.normalizedCarPosition[0];
            for (size_t j = 1; j < lap_data.normalizedCarPosition.size(); ++j) {
                float curr_pos = lap_data.normalizedCarPosition[j];
                if (curr_pos - prev_pos < -0.5f) {
                    base_offset += 1.0f;
                } else if (curr_pos - prev_pos > 0.5f) {
                    base_offset -= 1.0f;
                }
                prev_pos = curr_pos;
                lap_data.normalizedCarPosition[j] = curr_pos + base_offset;
            }
        }
        
        loaded_session_data.push_back(lap_data);
    }
    infile.close();
    Dictionary ret = _calculate_session_metadata(loaded_session_static_data, count, loaded_session_data);
    ret["timestamp"] = dummy_ts;
    return ret;
}

Dictionary ACCProvider::get_session_metadata() {
    return _calculate_session_metadata(loaded_session_static_data, loaded_session_lap_count, loaded_session_data);
}

void ACCProvider::close_session() {
    loaded_session_data.clear();
    loaded_session_data.shrink_to_fit();
    loaded_session_lap_offsets.clear();
    loaded_session_lap_offsets.shrink_to_fit();
    loaded_session_sample_interval = 0.0;
    loaded_session_samples_per_meter = 0.0;
    loaded_session_lap_count = -1;
    loaded_session_static_data = {};
}

String ACCProvider::get_internal_channel_name(const String& standard_name) {
    if (standard_name == "packet_id_physics") return "packetId_physics";
    if (standard_name == "packet_id_graphic") return "packetId_graphic";
    
    if (standard_name == "speed") return "speedKmh";
    if (standard_name == "rpm") return "rpms";
    if (standard_name == "throttle") return "gas";
    if (standard_name == "brake") return "brake";
    if (standard_name == "clutch") return "clutch";
    if (standard_name == "gear") return "gear";
    if (standard_name == "steer_angle") return "steerAngle";
    if (standard_name == "steer_velocity") return "steerVelo";
    if (standard_name == "combined_pedals") return "gasBrakeCombined";
    if (standard_name == "fuel") return "fuel";
    
    if (standard_name == "lateral_g") return "accG_x";
    if (standard_name == "acc_g_y") return "accG_y";
    if (standard_name == "longitudinal_g") return "accG_z";
    if (standard_name == "velocity_x") return "velocity_x";
    if (standard_name == "velocity_y") return "velocity_y";
    if (standard_name == "velocity_z") return "velocity_z";
    if (standard_name == "lateral_velocity") return "localVelocity_x";
    if (standard_name == "local_velocity_y") return "localVelocity_y";
    if (standard_name == "local_velocity_z") return "localVelocity_z";
    if (standard_name == "local_angular_vel_x") return "localAngularVel_x";
    if (standard_name == "yaw_rate") return "localAngularVel_y";
    if (standard_name == "local_angular_vel_z") return "localAngularVel_z";
    
    if (standard_name == "heading") return "heading";
    if (standard_name == "pitch") return "pitch";
    if (standard_name == "roll") return "roll";
    
    // tyres
    if (standard_name == "wheel_slip_fl") return "wheelSlip_fl";
    if (standard_name == "wheel_slip_fr") return "wheelSlip_fr";
    if (standard_name == "wheel_slip_rl") return "wheelSlip_rl";
    if (standard_name == "wheel_slip_rr") return "wheelSlip_rr";
    
    if (standard_name == "wheel_load_fl") return "wheelLoad_fl";
    if (standard_name == "wheel_load_fr") return "wheelLoad_fr";
    if (standard_name == "wheel_load_rl") return "wheelLoad_rl";
    if (standard_name == "wheel_load_rr") return "wheelLoad_rr";
    
    if (standard_name == "wheel_pressure_fl") return "wheelsPressure_fl";
    if (standard_name == "wheel_pressure_fr") return "wheelsPressure_fr";
    if (standard_name == "wheel_pressure_rl") return "wheelsPressure_rl";
    if (standard_name == "wheel_pressure_rr") return "wheelsPressure_rr";
    
    if (standard_name == "wheel_angular_speed_fl") return "wheelAngularSpeed_fl";
    if (standard_name == "wheel_angular_speed_fr") return "wheelAngularSpeed_fr";
    if (standard_name == "wheel_angular_speed_rl") return "wheelAngularSpeed_rl";
    if (standard_name == "wheel_angular_speed_rr") return "wheelAngularSpeed_rr";
    
    if (standard_name == "tyre_wear_fl") return "tyreWear_fl";
    if (standard_name == "tyre_wear_fr") return "tyreWear_fr";
    if (standard_name == "tyre_wear_rl") return "tyreWear_rl";
    if (standard_name == "tyre_wear_rr") return "tyreWear_rr";
    
    if (standard_name == "tyre_dirty_level_fl") return "tyreDirtyLevel_fl";
    if (standard_name == "tyre_dirty_level_fr") return "tyreDirtyLevel_fr";
    if (standard_name == "tyre_dirty_level_rl") return "tyreDirtyLevel_rl";
    if (standard_name == "tyre_dirty_level_rr") return "tyreDirtyLevel_rr";
    
    if (standard_name == "tyre_core_temp_fl") return "tyreCoreTemperature_fl";
    if (standard_name == "tyre_core_temp_fr") return "tyreCoreTemperature_fr";
    if (standard_name == "tyre_core_temp_rl") return "tyreCoreTemperature_rl";
    if (standard_name == "tyre_core_temp_rr") return "tyreCoreTemperature_rr";
    
    if (standard_name == "tyre_temp_i_fl") return "tyreTempI_fl";
    if (standard_name == "tyre_temp_i_fr") return "tyreTempI_fr";
    if (standard_name == "tyre_temp_i_rl") return "tyreTempI_rl";
    if (standard_name == "tyre_temp_i_rr") return "tyreTempI_rr";
    
    if (standard_name == "tyre_temp_m_fl") return "tyreTempM_fl";
    if (standard_name == "tyre_temp_m_fr") return "tyreTempM_fr";
    if (standard_name == "tyre_temp_m_rl") return "tyreTempM_rl";
    if (standard_name == "tyre_temp_m_rr") return "tyreTempM_rr";
    
    if (standard_name == "tyre_temp_o_fl") return "tyreTempO_fl";
    if (standard_name == "tyre_temp_o_fr") return "tyreTempO_fr";
    if (standard_name == "tyre_temp_o_rl") return "tyreTempO_rl";
    if (standard_name == "tyre_temp_o_rr") return "tyreTempO_rr";
    
    if (standard_name == "brake_temp_fl") return "brakeTemp_fl";
    if (standard_name == "brake_temp_fr") return "brakeTemp_fr";
    if (standard_name == "brake_temp_rl") return "brakeTemp_rl";
    if (standard_name == "brake_temp_rr") return "brakeTemp_rr";
    if (standard_name == "tyre_compound") return "tyreCompound";
    
    if (standard_name == "tyre_contact_point_fl_x") return "tyreContactPoint_fl_x";
    if (standard_name == "tyre_contact_point_fl_y") return "tyreContactPoint_fl_y";
    if (standard_name == "tyre_contact_point_fl_z") return "tyreContactPoint_fl_z";
    
    if (standard_name == "tyre_contact_point_fr_x") return "tyreContactPoint_fr_x";
    if (standard_name == "tyre_contact_point_fr_y") return "tyreContactPoint_fr_y";
    if (standard_name == "tyre_contact_point_fr_z") return "tyreContactPoint_fr_z";
    
    if (standard_name == "tyre_contact_point_rl_x") return "tyreContactPoint_rl_x";
    if (standard_name == "tyre_contact_point_rl_y") return "tyreContactPoint_rl_y";
    if (standard_name == "tyre_contact_point_rl_z") return "tyreContactPoint_rl_z";
    
    if (standard_name == "tyre_contact_point_rr_x") return "tyreContactPoint_rr_x";
    if (standard_name == "tyre_contact_point_rr_y") return "tyreContactPoint_rr_y";
    if (standard_name == "tyre_contact_point_rr_z") return "tyreContactPoint_rr_z";
    
    if (standard_name == "tyre_contact_normal_fl_x") return "tyreContactNormal_fl_x";
    if (standard_name == "tyre_contact_normal_fl_y") return "tyreContactNormal_fl_y";
    if (standard_name == "tyre_contact_normal_fl_z") return "tyreContactNormal_fl_z";
    
    if (standard_name == "tyre_contact_normal_fr_x") return "tyreContactNormal_fr_x";
    if (standard_name == "tyre_contact_normal_fr_y") return "tyreContactNormal_fr_y";
    if (standard_name == "tyre_contact_normal_fr_z") return "tyreContactNormal_fr_z";
    
    if (standard_name == "tyre_contact_normal_rl_x") return "tyreContactNormal_rl_x";
    if (standard_name == "tyre_contact_normal_rl_y") return "tyreContactNormal_rl_y";
    if (standard_name == "tyre_contact_normal_rl_z") return "tyreContactNormal_rl_z";
    
    if (standard_name == "tyre_contact_normal_rr_x") return "tyreContactNormal_rr_x";
    if (standard_name == "tyre_contact_normal_rr_y") return "tyreContactNormal_rr_y";
    if (standard_name == "tyre_contact_normal_rr_z") return "tyreContactNormal_rr_z";
    
    if (standard_name == "tyre_contact_heading_fl_x") return "tyreContactHeading_fl_x";
    if (standard_name == "tyre_contact_heading_fl_y") return "tyreContactHeading_fl_y";
    if (standard_name == "tyre_contact_heading_fl_z") return "tyreContactHeading_fl_z";
    
    if (standard_name == "tyre_contact_heading_fr_x") return "tyreContactHeading_fr_x";
    if (standard_name == "tyre_contact_heading_fr_y") return "tyreContactHeading_fr_y";
    if (standard_name == "tyre_contact_heading_fr_z") return "tyreContactHeading_fr_z";
    
    if (standard_name == "tyre_contact_heading_rl_x") return "tyreContactHeading_rl_x";
    if (standard_name == "tyre_contact_heading_rl_y") return "tyreContactHeading_rl_y";
    if (standard_name == "tyre_contact_heading_rl_z") return "tyreContactHeading_rl_z";
    
    if (standard_name == "tyre_contact_heading_rr_x") return "tyreContactHeading_rr_x";
    if (standard_name == "tyre_contact_heading_rr_y") return "tyreContactHeading_rr_y";
    if (standard_name == "tyre_contact_heading_rr_z") return "tyreContactHeading_rr_z";
    
    if (standard_name == "camber_deg_fl") return "camberDEG_fl";
    if (standard_name == "camber_deg_fr") return "camberDEG_fr";
    if (standard_name == "camber_deg_rl") return "camberDEG_rl";
    if (standard_name == "camber_deg_rr") return "camberDEG_rr";
    
    if (standard_name == "susp_travel_fl") return "suspensionTravel_fl";
    if (standard_name == "susp_travel_fr") return "suspensionTravel_fr";
    if (standard_name == "susp_travel_rl") return "suspensionTravel_rl";
    if (standard_name == "susp_travel_rr") return "suspensionTravel_rr";
    
    if (standard_name == "damper_vel_fl") return "damperVelocity_fl";
    if (standard_name == "damper_vel_fr") return "damperVelocity_fr";
    if (standard_name == "damper_vel_rl") return "damperVelocity_rl";
    if (standard_name == "damper_vel_rr") return "damperVelocity_rr";
    
    if (standard_name == "ride_height_f") return "rideHeight_f";
    if (standard_name == "ride_height_r") return "rideHeight_r";
    if (standard_name == "cg_height") return "cgHeight";
    
    // systems
    if (standard_name == "drs") return "drs";
    if (standard_name == "tc") return "tc";
    if (standard_name == "abs") return "abs";
    if (standard_name == "brake_bias_raw") return "brakeBias_raw";
    if (standard_name == "brake_bias") return "brakeBias";
    if (standard_name == "kers_charge") return "kersCharge";
    if (standard_name == "kers_input") return "kersInput";
    if (standard_name == "auto_shifter_on") return "autoShifterOn";
    if (standard_name == "turbo_boost") return "turboBoost";
    if (standard_name == "engine_brake") return "engineBrake";
    if (standard_name == "ers_recovery_level") return "ersRecoveryLevel";
    if (standard_name == "ers_power_level") return "ersPowerLevel";
    if (standard_name == "ers_heat_charging") return "ersHeatCharging";
    if (standard_name == "ers_is_charging") return "ersIsCharging";
    if (standard_name == "kers_current_kj") return "kersCurrentKJ";
    if (standard_name == "drs_available") return "drsAvailable";
    if (standard_name == "drs_enabled") return "drsEnabled";
    if (standard_name == "pit_limiter_on") return "pitLimiterOn";
    
    // environment & session
    if (standard_name == "air_density") return "airDensity";
    if (standard_name == "air_temp") return "airTemp";
    if (standard_name == "road_temp") return "roadTemp";
    if (standard_name == "surface_grip") return "surfaceGrip";
    if (standard_name == "wind_speed") return "windSpeed";
    if (standard_name == "wind_direction") return "windDirection";
    if (standard_name == "session_time_left") return "sessionTimeLeft";
    if (standard_name == "session_status") return "status";
    if (standard_name == "session_type") return "session";
    if (standard_name == "timestamp") return "timestamp";
    
    if (standard_name == "normalized_car_position") return "normalizedCarPosition";
    if (standard_name == "distance_traveled") return "distanceTraveled";
    if (standard_name == "position") return "position";
    if (standard_name == "is_in_pit") return "isInPit";
    if (standard_name == "is_in_pit_lane") return "isInPitLane";
    if (standard_name == "current_sector") return "currentSectorIndex";
    if (standard_name == "last_sector_time") return "lastSectorTime";
    if (standard_name == "penalty_time") return "penaltyTime";
    if (standard_name == "flag") return "flag";
    if (standard_name == "mandatory_pit_done") return "mandatoryPitDone";
    if (standard_name == "is_ai_controlled") return "isAIControlled";
    
    if (standard_name == "current_time") return "currentTime";
    if (standard_name == "last_time") return "lastTime";
    if (standard_name == "best_time") return "bestTime";
    if (standard_name == "split") return "split";
    if (standard_name == "i_current_time") return "iCurrentTime";
    if (standard_name == "i_last_time") return "iLastTime";
    if (standard_name == "i_best_time") return "iBestTime";
    if (standard_name == "number_of_laps") return "numberOfLaps";
    if (standard_name == "completed_laps") return "completedLaps";
    if (standard_name == "replay_time_multiplier") return "replayTimeMultiplier";
    
    if (standard_name == "car_coords_x") return "carCoordinates_x";
    if (standard_name == "car_coords_y") return "carCoordinates_y";
    if (standard_name == "car_coords_z") return "carCoordinates_z";
    if (standard_name == "car_coordinates") return "carCoordinates";
    if (standard_name == "car_damage_front") return "carDamage_0";
    if (standard_name == "car_damage_rear") return "carDamage_1";
    if (standard_name == "car_damage_left") return "carDamage_2";
    if (standard_name == "car_damage_right") return "carDamage_3";
    if (standard_name == "car_damage_center") return "carDamage_4";
    if (standard_name == "tyres_out") return "numberOfTyresOut";

    // acc mapping
    if (standard_name == "tc_in_action") return "tcinAction";
    if (standard_name == "abs_in_action") return "absInAction";
    if (standard_name == "water_temp") return "waterTemp";
    if (standard_name == "engine_map") return "EngineMap";
    if (standard_name == "tc_cut") return "TCCUT";
    if (standard_name == "exhaust_temp") return "exhaustTemperature";
    if (standard_name == "active_cars") return "activeCars";
    if (standard_name == "player_car_id") return "playerCarID";
    if (standard_name == "penalty") return "penalty";
    if (standard_name == "is_setup_menu_visible") return "isSetupMenuVisible";
    if (standard_name == "main_display_index") return "mainDisplayIndex";
    if (standard_name == "secondary_display_index") return "secondaryDisplyIndex";
    if (standard_name == "fuel_x_lap") return "fuelXLap";
    if (standard_name == "rain_lights") return "rainLights";
    if (standard_name == "flashing_lights") return "flashingLights";
    if (standard_name == "lights_stage") return "lightsStage";
    if (standard_name == "wiper_lv") return "wiperLV";
    if (standard_name == "driver_stint_total_time_left") return "driverStintTotalTimeLeft";
    if (standard_name == "driver_stint_time_left") return "driverStintTimeLeft";
    if (standard_name == "rain_tyres") return "rainTyres";
    if (standard_name == "session_index") return "sessionIndex";
    if (standard_name == "used_fuel") return "usedFuel";
    if (standard_name == "delta_lap_time") return "deltaLapTime";
    if (standard_name == "i_delta_lap_time") return "iDeltaLapTime";
    if (standard_name == "estimated_lap_time") return "estimatedLapTime";
    if (standard_name == "i_estimated_lap_time") return "iEstimatedLapTime";
    if (standard_name == "is_delta_positive") return "isDeltaPositive";
    if (standard_name == "i_split") return "iSplit";
    if (standard_name == "is_valid_lap") return "isValidLap";
    if (standard_name == "fuel_estimated_laps") return "fuelEstimatedLaps";
    if (standard_name == "track_status") return "trackStatus";
    if (standard_name == "missing_mandatory_pits") return "missingMandatoryPits";
    if (standard_name == "clock") return "Clock";
    if (standard_name == "direction_lights_left") return "directionLightsLeft";
    if (standard_name == "direction_lights_right") return "directionLightsRight";
    if (standard_name == "global_yellow") return "GlobalYellow";
    if (standard_name == "global_yellow1") return "GlobalYellow1";
    if (standard_name == "global_yellow2") return "GlobalYellow2";
    if (standard_name == "global_yellow3") return "GlobalYellow3";
    if (standard_name == "global_white") return "GlobalWhite";
    if (standard_name == "global_green") return "GlobalGreen";
    if (standard_name == "global_chequered") return "GlobalChequered";
    if (standard_name == "global_red") return "GlobalRed";
    if (standard_name == "mfd_tyre_set") return "mfdTyreSet";
    if (standard_name == "mfd_fuel_to_add") return "mfdFuelToAdd";
    if (standard_name == "mfd_tyre_pressure_lf") return "mfdTyrePressureLF";
    if (standard_name == "mfd_tyre_pressure_rf") return "mfdTyrePressureRF";
    if (standard_name == "mfd_tyre_pressure_lr") return "mfdTyrePressureLR";
    if (standard_name == "mfd_tyre_pressure_rr") return "mfdTyrePressureRR";
    if (standard_name == "track_grip_status") return "trackGripStatus";
    if (standard_name == "rain_intensity") return "rainIntensity";
    if (standard_name == "rain_intensity_in_10min") return "rainIntensityIn10min";
    if (standard_name == "rain_intensity_in_30min") return "rainIntensityIn30min";
    if (standard_name == "current_tyre_set") return "currentTyreSet";
    if (standard_name == "strategy_tyre_set") return "strategyTyreSet";
    if (standard_name == "gap_ahead") return "gapAhead";
    if (standard_name == "gap_behind") return "gapBehind";

    if (standard_name == "mz_fl") return "mz_FL";
    if (standard_name == "mz_fr") return "mz_FR";
    if (standard_name == "mz_rl") return "mz_RL";
    if (standard_name == "mz_rr") return "mz_RR";

    if (standard_name == "fx_fl") return "fx_FL";
    if (standard_name == "fx_fr") return "fx_FR";
    if (standard_name == "fx_rl") return "fx_RL";
    if (standard_name == "fx_rr") return "fx_RR";

    if (standard_name == "fy_fl") return "fy_FL";
    if (standard_name == "fy_fr") return "fy_FR";
    if (standard_name == "fy_rl") return "fy_RL";
    if (standard_name == "fy_rr") return "fy_RR";

    if (standard_name == "slip_ratio_fl") return "slipRatio_FL";
    if (standard_name == "slip_ratio_fr") return "slipRatio_FR";
    if (standard_name == "slip_ratio_rl") return "slipRatio_RL";
    if (standard_name == "slip_ratio_rr") return "slipRatio_RR";

    if (standard_name == "slip_angle_fl") return "slipAngle_FL";
    if (standard_name == "slip_angle_fr") return "slipAngle_FR";
    if (standard_name == "slip_angle_rl") return "slipAngle_RL";
    if (standard_name == "slip_angle_rr") return "slipAngle_RR";

    if (standard_name == "susp_damage_fl") return "suspensionDamage_FL";
    if (standard_name == "susp_damage_fr") return "suspensionDamage_FR";
    if (standard_name == "susp_damage_rl") return "suspensionDamage_RL";
    if (standard_name == "susp_damage_rr") return "suspensionDamage_RR";

    if (standard_name == "tyre_temp_fl") return "tyreTemp_FL";
    if (standard_name == "tyre_temp_fr") return "tyreTemp_FR";
    if (standard_name == "tyre_temp_rl") return "tyreTemp_RL";
    if (standard_name == "tyre_temp_rr") return "tyreTemp_RR";

    if (standard_name == "brake_pressure_fl") return "brakePressure_FL";
    if (standard_name == "brake_pressure_fr") return "brakePressure_FR";
    if (standard_name == "brake_pressure_rl") return "brakePressure_RL";
    if (standard_name == "brake_pressure_rr") return "brakePressure_RR";

    if (standard_name == "pad_life_fl") return "padLife_FL";
    if (standard_name == "pad_life_fr") return "padLife_FR";
    if (standard_name == "pad_life_rl") return "padLife_RL";
    if (standard_name == "pad_life_rr") return "padLife_RR";

    if (standard_name == "disc_life_fl") return "discLife_FL";
    if (standard_name == "disc_life_fr") return "discLife_FR";
    if (standard_name == "disc_life_rl") return "discLife_RL";
    if (standard_name == "disc_life_rr") return "discLife_RR";

    if (standard_name == "kerb_vibration") return "kerbVibration";
    if (standard_name == "slip_vibrations") return "slipVibrations";
    if (standard_name == "g_vibrations") return "gVibrations";
    if (standard_name == "abs_vibrations") return "absVibrations";

    return standard_name; // fallback
}

String ACCProvider::_open_session_file(const String& file_path, TelemetryFile& infile, ACC_SPageStatic& out_static, double& out_sample_interval, double& out_samples_per_meter, uint64_t& out_lap_count, std::vector<uint64_t>& out_lap_offsets, int64_t& out_timestamp) {
    if (file_path.is_empty()) return "file path is empty";

    if (!infile.open_read(file_path)) return "could not open file: " + file_path;

    // check signature
    int sig_len = save_file_signature.utf8().length();
    std::vector<char> sig_buffer(sig_len);
    infile.read(sig_buffer.data(), sig_len);
    
    String read_sig = String::utf8(sig_buffer.data(), sig_len);
    if (read_sig != save_file_signature) {
        infile.close();
        return "invalid signature";
    }

    // read timestamp
    infile.read(&out_timestamp, sizeof(int64_t));
    if (infile.fail()) {
        infile.close(); return "failed reading timestamp";
    }

    // read static data
    infile.read(&out_static, sizeof(ACC_SPageStatic));
    if (infile.fail()) {
        infile.close(); return "failed reading static data";
    }
    
    // read sample interval
    infile.read(&out_sample_interval, sizeof(double));
    if (infile.fail()) {
        infile.close(); return "failed reading sample interval";
    }
    
    // read samples per meter
    infile.read(&out_samples_per_meter, sizeof(double));
    if (infile.fail()) {
        infile.close(); return "failed reading samples per meter";
    }

    // read total laps count
    infile.read(&out_lap_count, sizeof(uint64_t));
    if (infile.fail()) {
        infile.close(); return "failed reading lap count";
    }
    
    // read lap offsets
    out_lap_offsets.resize(out_lap_count);
    if (out_lap_count > 0) {
        infile.read(out_lap_offsets.data(), out_lap_count * sizeof(uint64_t));
        if (infile.fail()) {
            infile.close(); return "failed reading lap offsets";
        }
    }

    return "";
}

Dictionary ACCProvider::_calculate_session_metadata(const ACC_SPageStatic& stat, uint64_t count, std::vector<ACC_LapDataChannels>& laps) {
    Dictionary meta;
    double track_length = get_acc_track_length(wchar_to_gdstring(stat.track, 33).to_lower());

    meta["sim_id"] = "ACC";
    meta["track_name"] = wchar_to_gdstring(stat.track, 33).to_lower();
    meta["track_config"] = ""; // NOTE: 'trackConfiguration' is not used by acc
    meta["car_name"] = wchar_to_gdstring(stat.carModel, 33);
    meta["total_laps"] = (int)count;
    meta["sector_count"] = stat.sectorCount;
    
    meta["session_type"] = -1;
    for (uint64_t i = 0; i < count; i++) {
        if (!laps[i].session.empty()) {
            meta["session_type"] = laps[i].session[0];
            break;
        }
    }

    Array laps_arr;
    int best_lap_time = 0;
    Dictionary sector_positions_norm;
    Dictionary sector_positions_m;

    std::vector<float> boundaries = get_acc_sectors(wchar_to_gdstring(stat.track, 33).to_lower());
    for (size_t s = 0; s < boundaries.size(); s++) {
        sector_positions_norm[s] = boundaries[s];
        sector_positions_m[s] = boundaries[s] * track_length;
    }
    
    std::map<int, Dictionary> all_sector_times;
    
    for (uint64_t i = 0; i < count; i++) {
        const ACC_LapDataChannels& lap = laps[i];
        if (lap.speedKmh.empty()) continue;
        
        int current_sector = 0;
        int last_boundary_time = 0;
        
        for (size_t j = 0; j < lap.currentSectorIndex.size(); j++) {
            if (lap.currentSectorIndex[j] > current_sector) {
                int sector_time = lap.iCurrentTime[j] - last_boundary_time;
                if (sector_time > 0) {
                    if (!all_sector_times.count(i)) all_sector_times[i] = Dictionary();
                    all_sector_times[i][current_sector] = sector_time;
                }
                last_boundary_time = lap.iCurrentTime[j];
                current_sector = lap.currentSectorIndex[j];
            }
        }
    }
    
    for (uint64_t i = 0; i < count; i++) {
        const ACC_LapDataChannels& lap = laps[i];

        if (lap.speedKmh.empty()) continue;

        Dictionary lap_stats;
        int lap_time = 0;
        Dictionary sector_times;
        if (all_sector_times.count(i)) {
            sector_times = all_sector_times[i];
        }
        float top_speed = 0.0f;
        
        std::map<int, float> sec_vmax;
        std::map<int, float> sec_vmin;
        std::map<int, float> sec_speed_sum;
        std::map<int, int> sec_snapshot_count;
        std::map<int, int> sec_throttle_count;
        std::map<int, int> sec_brake_count;

        for (size_t j = 0; j < lap.speedKmh.size(); j++) {
            int loop_sec_idx = lap.currentSectorIndex.empty() ? 0 : lap.currentSectorIndex[j];
            
            float speed = lap.speedKmh[j];
            float gas = j < lap.gas.size() ? lap.gas[j] : 0.0f;
            float brake = j < lap.brake.size() ? lap.brake[j] : 0.0f;

            if (!sec_vmax.count(loop_sec_idx)) {
                sec_vmax[loop_sec_idx] = speed;
                sec_vmin[loop_sec_idx] = speed;
                sec_speed_sum[loop_sec_idx] = 0.0f;
                sec_snapshot_count[loop_sec_idx] = 0;
                sec_throttle_count[loop_sec_idx] = 0;
                sec_brake_count[loop_sec_idx] = 0;
            }

            if (speed > sec_vmax[loop_sec_idx]) sec_vmax[loop_sec_idx] = speed;
            if (speed < sec_vmin[loop_sec_idx]) sec_vmin[loop_sec_idx] = speed;
            sec_speed_sum[loop_sec_idx] += speed;
            sec_snapshot_count[loop_sec_idx]++;
            
            if (gas > 0.05f) sec_throttle_count[loop_sec_idx]++;
            if (brake > 0.05f) sec_brake_count[loop_sec_idx]++;

            if (speed > top_speed) {
                top_speed = speed;
            }
        }

        Dictionary sectors_data;
        for (const auto& pair : sec_snapshot_count) {
            int s_idx = pair.first;
            int count = pair.second;
            if (count == 0) continue;
            
            Dictionary s_dict;
            s_dict["vmax_kmh"] = sec_vmax[s_idx];
            s_dict["vmin_kmh"] = sec_vmin[s_idx];
            s_dict["avg_speed_kmh"] = sec_speed_sum[s_idx] / count;
            s_dict["throttle_pct"] = (float)sec_throttle_count[s_idx] / count * 100.0f;
            s_dict["brake_pct"] = (float)sec_brake_count[s_idx] / count * 100.0f;
            
            sectors_data[s_idx] = s_dict;
        }
        


        bool is_completed = false;
        if (lap.normalizedCarPosition.size() > 1) {
            float total_progression = 0.0f;
            for (size_t k = 1; k < lap.normalizedCarPosition.size(); k++) {
                float diff = lap.normalizedCarPosition[k] - lap.normalizedCarPosition[k-1];
                if (diff < -0.5f) diff += 1.0f;
                if (diff > 0.5f) diff -= 1.0f;
                
                if (diff > 0.0f && diff < 0.1f) {
                    total_progression += diff;
                }
            }
            if (total_progression > 0.85f) {
                if (i < count - 1) {
                    float last_pos = lap.normalizedCarPosition.back();
                    float next_pos = laps[i + 1].normalizedCarPosition.front();
                    // check if it wrapped around the finish line
                    if (last_pos > 0.90f && next_pos < 0.10f) {
                        is_completed = true;
                    }
                }
            }
        }

        bool is_valid = is_completed;
        if (is_valid && !lap.isValidLap.empty() && !lap.iCurrentTime.empty()) {
            bool found_invalid = false;
            for (size_t k = 0; k < lap.isValidLap.size(); k++) {
                if (lap.isValidLap[k] == 0) {
                    found_invalid = true;
                    break;
                }
            }
            if (found_invalid) {
                is_valid = false;
            }
            
            if (is_valid) {
                size_t p_size = lap.penaltyTime.size();
                size_t f_size = lap.flag.size();
                size_t pit_size = lap.isInPitLane.size();
                size_t min_size = std::min({p_size, f_size, pit_size});
                
                for (size_t k = 0; k < min_size; k++) {
                    if (lap.penaltyTime[k] > 0.0f || lap.flag[k] == ACC_PENALTY_FLAG || lap.isInPitLane[k] == 1) {
                        is_valid = false;
                        break;
                    }
                }
            }
        }

        int next_lap_best_time = 0;
        // try getting exact time from next lap if exists
        int real_lap_time = 0;
        if (i + 1 < count) {
            const ACC_LapDataChannels& next_lap = laps[i + 1];
            
            if (!next_lap.iBestTime.empty()) {
                next_lap_best_time = next_lap.iBestTime[0];
            }

            // timing line and position wrap are often slightly offset.
            // the very first samples of next_lap might still hold the previous lap's time,
            // and the very last samples might hold next_lap's time.
            // however, the vast majority of next_lap's samples will hold this lap's true time.
            // we extract it by finding the most frequent valid iLastTime in next_lap.
            if (!next_lap.iLastTime.empty()) {
                std::map<int, int> counts;
                int max_count = 0;
                int next_most_frequent = 0;
                for (int t : next_lap.iLastTime) {
                    if (t > 0 && t < INT_MAX) {
                        counts[t]++;
                        if (counts[t] > max_count) {
                            max_count = counts[t];
                            next_most_frequent = t;
                        }
                    }
                }

                int curr_most_frequent = 0;
                if (!lap.iLastTime.empty()) {
                    std::map<int, int> curr_counts;
                    int curr_max = 0;
                    for (int t : lap.iLastTime) {
                        if (t > 0 && t < INT_MAX) {
                            curr_counts[t]++;
                            if (curr_counts[t] > curr_max) {
                                curr_max = curr_counts[t];
                                curr_most_frequent = t;
                            }
                        }
                    }
                }

                if (next_most_frequent > 0) {
                    // if the game didn't update iLastTime across the finish line,
                    // it usually means telemetry is stuck or lap was secretly rejected.
                    // however, we cross-reference with the physical elapsed time to ensure
                    // they didn't just drive the exact same millisecond.
                    if (curr_most_frequent > 0 && next_most_frequent == curr_most_frequent) {
                        int max_current_time = 0;
                        for (int t : lap.iCurrentTime) {
                            if (t > max_current_time && t < INT_MAX) max_current_time = t;
                        }
                        
                        // if the stuck time is vastly different
                        // from the true elapsed time, it's a bug.
                        if (std::abs(next_most_frequent - max_current_time) > 50) {
                            is_valid = false;
                        } else {
                            real_lap_time = next_most_frequent;
                        }
                    } else {
                        real_lap_time = next_most_frequent;
                    }
                }
            }
        }
        
        if (real_lap_time > 0) {
            lap_time = real_lap_time;
        } else if (!lap.iCurrentTime.empty()) {
            // fallback to max iCurrentTime value in this lap's data.
            // we can't use .back() because acc resets iCurrentTime ~25m before
            // our lap boundary (position wrap), so the last samples are near-zero.
            // max() safely captures the elapsed time before acc's internal reset.
            int max_time = 0;
            for (int t : lap.iCurrentTime) {
                if (t > max_time && t < INT_MAX) max_time = t;
            }
            lap_time = max_time;
        } else {
            lap_time = 0; // Invalid lap
            is_completed = false;
        }

        // calculate final sector time geometrically
        int last_sec_idx = (stat.sectorCount > 0) ? stat.sectorCount - 1 : 2;
        if (lap_time > 0 && !sector_times.has(last_sec_idx)) {
            int accumulated_time = 0;
            bool all_prev_exist = true;
            for (int s = 0; s < last_sec_idx; s++) {
                if (sector_times.has(s)) {
                    accumulated_time += (int)sector_times[s];
                } else {
                    all_prev_exist = false;
                }
            }
            if (all_prev_exist && accumulated_time > 0 && lap_time > accumulated_time) {
                sector_times[last_sec_idx] = lap_time - accumulated_time;
            }
        }
        
        lap_stats["lap_time_ms"] = lap_time;
        lap_stats["sector_times_ms"] = sector_times;
        lap_stats["top_speed_kmh"] = top_speed;
        lap_stats["snapshot_count"] = (int)lap.speedKmh.size();
        lap_stats["is_completed"] = is_completed;
        lap_stats["is_valid"] = is_valid;
        lap_stats["sectors_data"] = sectors_data;

        laps_arr.push_back(lap_stats);

        if (is_valid && lap_time > 0 && (best_lap_time == 0 || lap_time < best_lap_time)) {
            best_lap_time = lap_time;
        }
    }

    Dictionary best_sectors_dict;
    for (int i = 0; i < laps_arr.size(); i++) {
        Dictionary lap_dict = laps_arr[i];
        if (!lap_dict["is_completed"]) continue;
        
        Dictionary sectors = lap_dict["sector_times_ms"];
        Array keys = sectors.keys();
        for (int k = 0; k < keys.size(); k++) {
            int sec_idx = keys[k];
            int time = sectors[sec_idx];
            if (time <= 0) continue;
            
            if (!best_sectors_dict.has(sec_idx) || time < (int)best_sectors_dict[sec_idx]) {
                best_sectors_dict[sec_idx] = time;
            }
        }
    }

    meta["best_lap_time"] = best_lap_time;
    meta["best_sectors"] = best_sectors_dict;
    meta["sector_positions_norm"] = sector_positions_norm;
    meta["sector_positions_m"] = sector_positions_m;
    meta["laps"] = laps_arr;

    return meta;
}

Dictionary ACCProvider::_static_to_dict(const ACC_SPageStatic &s) {
    Dictionary dict;
    
    // raw acc keys
    dict["smVersion"] = wchar_to_gdstring(s.smVersion, 15);
    dict["acVersion"] = wchar_to_gdstring(s.acVersion, 15);
    dict["numberOfSessions"] = s.numberOfSessions;
    dict["numCars"] = s.numCars;
    dict["playerName"] = wchar_to_gdstring(s.playerName, 33);
    dict["playerSurname"] = wchar_to_gdstring(s.playerSurname, 33);
    dict["playerNick"] = wchar_to_gdstring(s.playerNick, 33);
    dict["maxTorque"] = s.maxTorque;
    dict["maxPower"] = s.maxPower;
    dict["maxFuel"] = s.maxFuel;
    dict["suspensionMaxTravel"] = Vector4(s.suspensionMaxTravel[0], s.suspensionMaxTravel[1], s.suspensionMaxTravel[2], s.suspensionMaxTravel[3]);
    dict["tyreRadius"] = Vector4(s.tyreRadius[0], s.tyreRadius[1], s.tyreRadius[2], s.tyreRadius[3]);
    dict["maxTurboBoost"] = s.maxTurboBoost;
    dict["penaltiesEnabled"] = s.penaltiesEnabled;
    dict["aidFuelRate"] = s.aidFuelRate;
    dict["aidTireRate"] = s.aidTireRate;
    dict["aidMechanicalDamage"] = s.aidMechanicalDamage;
    dict["aidAllowTyreBlankets"] = s.aidAllowTyreBlankets;
    dict["aidStability"] = s.aidStability;
    dict["aidAutoClutch"] = s.aidAutoClutch;
    dict["aidAutoBlip"] = s.aidAutoBlip;
    dict["hasDRS"] = s.hasDRS;
    dict["hasERS"] = s.hasERS;
    dict["hasKERS"] = s.hasKERS;
    dict["kersMaxJ"] = s.kersMaxJ;
    dict["engineBrakeSettingsCount"] = s.engineBrakeSettingsCount;
    dict["ersPowerControllerCount"] = s.ersPowerControllerCount;
    dict["ersMaxJ"] = s.ersMaxJ;
    dict["isTimedRace"] = s.isTimedRace;
    dict["hasExtraLap"] = s.hasExtraLap;
    dict["carSkin"] = ""; // NOTE: 'carSkin' is not used by acc
    dict["reversedGridPositions"] = s.reversedGridPositions;
    dict["pitWindowStart"] = s.pitWindowStart;
    dict["pitWindowEnd"] = s.pitWindowEnd;
    dict["isOnline"] = s.isOnline;
    dict["dryTyresName"] = wchar_to_gdstring(s.dryTyresName, 33);
    dict["wetTyresName"] = wchar_to_gdstring(s.wetTyresName, 33);

    // standard multi-sim keys
    // TODO: it can be expanded later
    dict["car_model"] = wchar_to_gdstring(s.carModel, 33);
    dict["track_name"] = wchar_to_gdstring(s.track, 33).to_lower();
    dict["track_config"] = ""; // NOTE: 'trackConfiguration' is not used by acc
    dict["track_length"] = get_acc_track_length(dict["track_name"]);
    dict["max_rpm"] = s.maxRpm;
    dict["sector_count"] = s.sectorCount;
    
    return dict;
}

float ACCProvider::get_acc_bbias_offset(const String& car_model) const {
    // gt3 - 2018
    if (car_model == "amr_v12_vantage_gt3") return -7.0f;
    else if (car_model == "audi_r8_lms") return -14.0f;
    else if (car_model == "bentley_continental_gt3_2016") return -7.0f;
    else if (car_model == "bentley_continental_gt3_2018") return -7.0f;
    else if (car_model == "bmw_m6_gt3") return -15.0f;
    else if (car_model == "jaguar_g3") return -7.0f;
    else if (car_model == "ferrari_488_gt3") return -17.0f;
    else if (car_model == "honda_nsx_gt3") return -14.0f;
    else if (car_model == "lamborghini_gallardo_rex") return -14.0f;
    else if (car_model == "lamborghini_huracan_gt3") return -14.0f;
    else if (car_model == "lamborghini_huracan_st") return -14.0f;
    else if (car_model == "lexus_rc_f_gt3") return -14.0f;
    else if (car_model == "mclaren_650s_gt3") return -17.0f;
    else if (car_model == "mercedes_amg_gt3") return -14.0f;
    else if (car_model == "nissan_gt_r_gt3_2017") return -15.0f;
    else if (car_model == "nissan_gt_r_gt3_2018") return -15.0f;
    else if (car_model == "porsche_991_gt3_r") return -21.0f;
    else if (car_model == "porsche_991ii_gt3_cup") return -5.0f;
    
    // gt3 - 2019
    else if (car_model == "amr_v8_vantage_gt3") return -7.0f;
    else if (car_model == "audi_r8_lms_evo") return -14.0f;
    else if (car_model == "honda_nsx_gt3_evo") return -14.0f;
    else if (car_model == "lamborghini_huracan_gt3_evo") return -14.0f;
    else if (car_model == "mclaren_720s_gt3") return -17.0f;
    else if (car_model == "porsche_991ii_gt3_r") return -21.0f;
    
    // gt4
    else if (car_model == "alpine_a110_gt4") return -15.0f;
    else if (car_model == "amr_v8_vantage_gt4") return -20.0f;
    else if (car_model == "audi_r8_gt4") return -15.0f;
    else if (car_model == "bmw_m4_gt4") return -22.0f;
    else if (car_model == "chevrolet_camaro_gt4r") return -18.0f;
    else if (car_model == "ginetta_g55_gt4") return -18.0f;
    else if (car_model == "ktm_xbow_gt4") return -20.0f;
    else if (car_model == "maserati_mc_gt4") return -15.0f;
    else if (car_model == "mclaren_570s_gt4") return -9.0f;
    else if (car_model == "mercedes_amg_gt4") return -20.0f;
    else if (car_model == "porsche_718_cayman_gt4_mr") return -20.0f;
    
    // gt3 - 2020
    else if (car_model == "ferrari_488_gt3_evo") return -17.0f;
    else if (car_model == "mercedes_amg_gt3_evo") return -14.0f;
    
    // gt3- 2021
    else if (car_model == "bmw_m4_gt3") return -14.0f;

    // gt3 - 2023
    else if (car_model == "mclaren_720s_gt3_evo") return -17.0f;
    else if (car_model == "porsche_992_gt3_r") return -21.0f;
    else if (car_model == "lamborghini_huracan_gt3_evo2") return -14.0f;
    else if (car_model == "ferrari_296_gt3") return -17.0f;
    
    // gt3 - 2024
    else if (car_model == "ford_mustang_gt3") return -14.0f;
    
    // challengers pack - 2022
    else if (car_model == "audi_r8_lms_evo_ii") return -14.0f;
    else if (car_model == "bmw_m2_cs_racing") return -17.0f;
    else if (car_model == "ferrari_488_challenge_evo") return -13.0f;
    else if (car_model == "lamborghini_huracan_st_evo2") return -14.0f;
    else if (car_model == "porsche_992_gt3_cup") return -5.0f;

    // gt2 pack - 2024
    else if (car_model == "audi_r8_lms_gt2") return -14.0f;
    else if (car_model == "ktm_xbow_gt2") return -20.0f;
    else if (car_model == "maserati_mc20_gt2") return -14.0f;
    else if (car_model == "mercedes_amg_gt2") return -14.0f;
    else if (car_model == "brabham_bt62_gt2") return -14.0f; // not in-game but sits in pak
    else if (car_model == "porsche_935") return -21.0f;
    else if (car_model == "porsche_991ii_gt2_rs_cs_evo") return -21.0f;
    
    return 0.0f;
}

float ACCProvider::get_acc_bpressure_multiplier(const String& car_model, const bool is_front) const {
    // gt3 - 2018
    if (car_model == "amr_v12_vantage_gt3") return 7.9585f;
    else if (car_model == "audi_r8_lms") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "bentley_continental_gt3_2016") return 7.9585f;
    else if (car_model == "bentley_continental_gt3_2018") return 7.9585f;
    else if (car_model == "bmw_m6_gt3") return 7.9585f;
    else if (car_model == "jaguar_g3") return 7.9585f;
    else if (car_model == "ferrari_488_gt3") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "honda_nsx_gt3") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_gallardo_rex") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_huracan_gt3") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_huracan_st") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lexus_rc_f_gt3") return 7.9585f;
    else if (car_model == "mclaren_650s_gt3") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "mercedes_amg_gt3") return 7.9585f;
    else if (car_model == "nissan_gt_r_gt3_2017") return 7.9585f;
    else if (car_model == "nissan_gt_r_gt3_2018") return 7.9585f;
    else if (car_model == "porsche_991_gt3_r") return is_front ? 7.1497f : 6.7715f;
    else if (car_model == "porsche_991ii_gt3_cup") return is_front ? 7.1497f : 6.7715f;
    
    // gt3 - 2019
    else if (car_model == "amr_v8_vantage_gt3") return 7.9585f;
    else if (car_model == "audi_r8_lms_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "honda_nsx_gt3_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_huracan_gt3_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "mclaren_720s_gt3") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "porsche_991ii_gt3_r") return is_front ? 7.1497f : 6.7715f;
    
    // gt4
    else if (car_model == "alpine_a110_gt4") return 10.0f;
    else if (car_model == "amr_v8_vantage_gt4") return 10.0f;
    else if (car_model == "audi_r8_gt4") return 10.0f;
    else if (car_model == "bmw_m4_gt4") return is_front ? 7.2886f : 10.0f;
    else if (car_model == "chevrolet_camaro_gt4r") return 10.0f;
    else if (car_model == "ginetta_g55_gt4") return 10.0f;
    else if (car_model == "ktm_xbow_gt4") return 10.0f;
    else if (car_model == "maserati_mc_gt4") return is_front ? 7.7768f : 7.6142f;
    else if (car_model == "mclaren_570s_gt4") return 10.0f;
    else if (car_model == "mercedes_amg_gt4") return 10.0f;
    else if (car_model == "porsche_718_cayman_gt4_mr") return 10.0f;
    
    // gt3 - 2020
    else if (car_model == "ferrari_488_gt3_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "mercedes_amg_gt3_evo") return 7.9585f;
    
    // gt3- 2021
    else if (car_model == "bmw_m4_gt3") return 7.9585f;

    // gt3 - 2023
    else if (car_model == "mclaren_720s_gt3_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_huracan_gt3_evo2") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "porsche_992_gt3_r") return is_front ? 7.1497f : 6.7715f;
    else if (car_model == "ferrari_296_gt3") return is_front ? 7.5980f : 7.4855f;

    // gt3 - 2024
    else if (car_model == "ford_mustang_gt3") return 7.9585f;
    
    // challengers pack - 2022
    else if (car_model == "audi_r8_lms_evo_ii") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "bmw_m2_cs_racing") return is_front ? 7.2886f : 10.0f;
    else if (car_model == "ferrari_488_challenge_evo") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "lamborghini_huracan_st_evo2") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "porsche_992_gt3_cup") return is_front ? 7.1497f : 6.7715f;

    // gt2 pack - 2024
    else if (car_model == "audi_r8_lms_gt2") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "ktm_xbow_gt2") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "maserati_mc20_gt2") return is_front ? 7.5980f : 7.4855f;
    else if (car_model == "mercedes_amg_gt2") return 7.9585f;
    else if (car_model == "brabham_bt62_gt2") return is_front ? 7.5980f : 7.4855f; // not in-game but sits in pak
    else if (car_model == "porsche_935") return is_front ? 7.1497f : 6.7715f;
    else if (car_model == "porsche_991ii_gt2_rs_cs_evo") return is_front ? 7.1497f : 6.7715f;
    
    return 7.9585f;
}

float ACCProvider::get_acc_track_length(const String& track_name) const {
    if (track_name == "barcelona") return 4655.0f;
    if (track_name == "brands_hatch") return 3908.0f;
    if (track_name == "hungaroring") return 4381.0f;
    if (track_name == "misano") return 4226.0f;
    if (track_name == "monza") return 5793.0f;
    if (track_name == "nurburgring") return 5137.0f;
    if (track_name == "paul_ricard") return 5770.0f;
    if (track_name == "silverstone") return 5891.0f;
    if (track_name == "spa") return 7004.0f;
    if (track_name == "zandvoort") return 4252.0f;
    if (track_name == "zolder") return 4011.0f;

    if (track_name == "imola") return 4909.0f;
    if (track_name == "donington") return 4020.0f;
    if (track_name == "oulton_park") return 4307.0f;
    if (track_name == "snetterton") return 4779.0f;
    if (track_name == "kyalami") return 4522.0f;
    if (track_name == "laguna_seca") return 3602.0f;
    if (track_name == "mount_panorama") return 6213.0f;
    if (track_name == "suzuka") return 5807.0f;
    if (track_name == "cota") return 5513.0f;
    if (track_name == "indianapolis") return 4167.0f;
    if (track_name == "watkins_glen") return 5552.0f;
    if (track_name == "valencia") return 4005.0f;
    if (track_name == "red_bull_ring") return 4318.0f;
    if (track_name == "nurburgring_24h") return 25378.0f;

    // fallback if somehow acc provides it
    if (dataStatic && dataStatic->trackSPlineLength > 0.0f) {
        return dataStatic->trackSPlineLength;
    }
    
    return 0.0f;
}

std::vector<float> ACCProvider::get_acc_sectors(const String& track_name) const {
    // real world sector distances in meters
    // (approximate to fia timing loops, acc spline might vary slightly)
    float track_len = get_acc_track_length(track_name);

    if (track_name == "barcelona") return {1528.0f / track_len, 3105.0f / track_len};
    if (track_name == "brands_hatch") return {1260.0f / track_len, 2595.0f / track_len};
    if (track_name == "hungaroring") return {1405.0f / track_len, 2871.0f / track_len};
    if (track_name == "misano") return {1425.0f / track_len, 2850.0f / track_len};
    if (track_name == "monza") return {1724.0f / track_len, 3877.0f / track_len};
    if (track_name == "nurburgring") return {1625.0f / track_len, 3355.0f / track_len};
    if (track_name == "paul_ricard") return {1875.0f / track_len, 3825.0f / track_len};
    if (track_name == "silverstone") return {1968.0f / track_len, 4165.0f / track_len};
    if (track_name == "spa") return {2225.0f / track_len, 5283.0f / track_len};
    if (track_name == "zandvoort") return {1440.0f / track_len, 2880.0f / track_len};
    if (track_name == "zolder") return {1333.0f / track_len, 2666.0f / track_len};

    if (track_name == "imola") return {1636.0f / track_len, 3272.0f / track_len};
    if (track_name == "donington") return {1340.0f / track_len, 2680.0f / track_len};
    if (track_name == "oulton_park") return {1435.0f / track_len, 2870.0f / track_len};
    if (track_name == "snetterton") return {1593.0f / track_len, 3186.0f / track_len};
    if (track_name == "kyalami") return {1507.0f / track_len, 3014.0f / track_len};
    if (track_name == "laguna_seca") return {1200.0f / track_len, 2400.0f / track_len};
    if (track_name == "mount_panorama") return {2071.0f / track_len, 4142.0f / track_len};
    if (track_name == "suzuka") return {1935.0f / track_len, 3870.0f / track_len};
    if (track_name == "cota") return {1837.0f / track_len, 3674.0f / track_len};
    if (track_name == "indianapolis") return {1397.0f / track_len, 2794.0f / track_len};
    if (track_name == "watkins_glen") return {1810.0f / track_len, 3620.0f / track_len};
    if (track_name == "valencia") return {1335.0f / track_len, 2670.0f / track_len};
    if (track_name == "red_bull_ring") return {1439.0f / track_len, 2878.0f / track_len};
    if (track_name == "nurburgring_24h") return {8459.0f / track_len, 16918.0f / track_len};
    
    return {1.0f/3.0f, 2.0f/3.0f}; // fallback for 3 sectors
}

void ACCProvider::logging_loop() {
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(sample_interval));
    auto next_tick = std::chrono::steady_clock::now();

    timeBeginPeriod(1);

    int stale_counter = 0;
    int graphic_stale_counter = 0;
    int local_last_graphic_packet_id = -1;
    int max_stale_ticks = static_cast<int>(3.0 / sample_interval); // 3 secs timeout

    wchar_t cached_track[33] = {0};
    double current_track_length = 0.0;
    
    // session tracking
    int current_session_type = -1;
    int current_session_index = -1;

    while (is_logging) {
        auto now = std::chrono::steady_clock::now();
        if (next_tick < now) {
            next_tick = now + interval;
        } else {
            next_tick += interval;
        }

        if (dataGraphic && dataPhysics) {
            
            // session auto-split check
            if (current_session_type == -1) {
                current_session_type = dataGraphic->session;
                current_session_index = dataGraphic->sessionIndex;
            } else if (dataGraphic->session != ACC_UNKNOWN && (dataGraphic->session != current_session_type || dataGraphic->sessionIndex != current_session_index)) {
                // session changed
                if (!sessions_data.empty()) {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    
                    int current_count = ++session_type_counts[current_session_type];
                    String session_suffix = _get_session_suffix(current_session_type);
                    if (current_count > 1) {
                        session_suffix += "_" + String::num_int64(current_count);
                    }

                    String output = session_output_file_path;
                    String extension = output.get_extension();
                    String base_path = output.get_basename();
                    String final_path = base_path + session_suffix + "." + extension;
                    
                    if (auto_save_callback) {
                        auto_save_callback(final_path);
                    }
                    
                    std::vector<ACC_LapDataChannels> data_to_save = std::move(sessions_data);
                    sessions_data.clear();
                    ACC_SPageStatic static_copy = *dataStatic;
                    double save_interval = sample_interval;
                    double save_spm = samples_per_meter;
                    
                    std::thread([this, data_to_save = std::move(data_to_save), static_copy, save_interval, save_spm, final_path]() mutable {
                        this->_flush_sessions_to_disk(std::move(data_to_save), static_copy, save_interval, save_spm, final_path);
                    }).detach();
                }
                
                current_session_type = dataGraphic->session;
                current_session_index = dataGraphic->sessionIndex;
                last_lap_count = 0;
                last_recorded_meter = -1.0;
            }

            if (dataGraphic->status == ACC_OFF) {
                game_disconnected.store(true);
            }

            // graphics packet updates as long as the game process is alive
            // so we're detecting game crash or close with this
            if (dataGraphic->packetId == local_last_graphic_packet_id) {
                graphic_stale_counter++;
                if (graphic_stale_counter > max_stale_ticks) {
                    game_disconnected.store(true);
                }
            } else {
                graphic_stale_counter = 0;
            }
            local_last_graphic_packet_id = dataGraphic->packetId;
            
            if (dataStatic) {
                if (wcsncmp(dataStatic->track, cached_track, 33) != 0) {
                    for (int i = 0; i < 33; ++i) cached_track[i] = dataStatic->track[i];
                    current_track_length = get_acc_track_length(wchar_to_gdstring(cached_track, 33).to_lower());
                }
            }

            // physics stale check         
            if (dataGraphic->status != ACC_LIVE && dataGraphic->status != ACC_REPLAY) {
                stale_counter = 0;
                std::this_thread::sleep_until(next_tick);
                continue;
            }

            if (dataPhysics->packetId == last_physics_packet_id) {
                std::this_thread::sleep_until(next_tick);
                continue;
            }
            int packet_diff_global = dataPhysics->packetId - last_physics_packet_id;
            last_physics_packet_id = dataPhysics->packetId;

            static int pause_recovery_counter = 0;
            
            // if resumed from pause or heavy lag, graphic and physics memory become desynced.
            if (packet_diff_global > 100 && !sessions_data.empty()) {
                pause_recovery_counter = 50; // ignore the next 50ms of data to allow them to resync
            }
            
            if (pause_recovery_counter > 0) {
                pause_recovery_counter--;
                continue;
            }
            
            // detect the exact moment of pausing where Physics instantly zeroes out but Graphic is still LIVE
            if (dataPhysics->rpms == 0 && dataPhysics->speedKmh == 0.0f && dataPhysics->gas == 0.0f) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(data_mutex);

                bool lap_changed = false;
                
                // prevent garage-to-pit teleportation jumps from ruining the out-lap chart
                // wait until the car actually moves before recording the first lap's samples
                if (sessions_data.empty() || sessions_data.back().timestamp.empty()) {
                    if (dataPhysics->speedKmh < 1.0f) {
                        continue;
                    }
                }
                
                
                static float last_graphic_norm_pos = 0.0f;
                float current_graphic_norm_pos = dataGraphic->normalizedCarPosition;

                // primary trigger: spline position wrap-around (1.0 -> 0.0)
                // we previously used completedLaps, but acc drops the out-lap count,
                // causing the out-lap and lap 1 to merge. the spline wrap-around
                // perfectly coincides with iCurrentTime reset and position wrap,
                // making it the most mathematically stable boundary for ui rendering.
                if (current_graphic_norm_pos < 0.05f && last_graphic_norm_pos > 0.95f) {
                    lap_changed = true;
                } else {
                    float diff = current_graphic_norm_pos - last_graphic_norm_pos;
                    if (diff < -0.5f) diff += 1.0f;
                    if (diff > 0.5f) diff -= 1.0f;
                    
                    float teleport_threshold = 0.2f;
                    if (current_track_length > 0) {
                        teleport_threshold = 200.0f / current_track_length; // 200 meters
                    }
                    
                    if (std::abs(diff) > teleport_threshold) {
                        lap_changed = true; // teleport detected
                    }
                }
                last_graphic_norm_pos = current_graphic_norm_pos;
                
                // still track completedLaps for metadata reference
                // even though it's not the trigger
                if (dataGraphic->completedLaps > last_lap_count) {
                    last_lap_count = dataGraphic->completedLaps;
                }

                // track iCurrentTime for metadata use
                last_i_current_time = dataGraphic->iCurrentTime;

                if (lap_changed || sessions_data.empty()) {
                    bool should_push = true;
                    if (!sessions_data.empty()) {
                        auto& last_lap = sessions_data.back();
                        if (last_lap.timestamp.size() < 10) {
                            should_push = false;
                        }
                    }

                    if (should_push) {
                        sessions_data.push_back(ACC_LapDataChannels());
                    } else {
                        sessions_data.back().clear();
                    }
                    last_recorded_meter = -INFINITY;
                }

                // track continuous spline distance
                // graphic position updates at 60Hz, physics at 400.
                // using graphic pos directly causes staircase artifacts.
                // we integrate physics speed for smooth high-frequency distance
                // and soft-correct it to graphic pos.
                static double internal_meter = -1.0;
                
                if (last_recorded_meter == -INFINITY) {
                    internal_meter = -1.0; // reset on new lap
                }
                
                double graphic_spline_pos = dataGraphic->normalizedCarPosition * current_track_length;
                
                if (internal_meter < 0.0 || current_track_length <= 0.0) {
                    internal_meter = graphic_spline_pos;
                } else {
                    int packet_diff = dataPhysics->packetId - last_physics_packet_id;
                    if (packet_diff > 0 && packet_diff < 100) {
                        double dt = packet_diff / 400.0; // 400Hz physics tick
                        internal_meter += (dataPhysics->speedKmh / 3.6) * dt;
                    }
                    
                    // complementary filter: soft-correct towards graphic_spline_pos
                    double diff = graphic_spline_pos - internal_meter;
                    // handle track wrap-around
                    if (diff < -current_track_length / 2.0) diff += current_track_length;
                    if (diff > current_track_length / 2.0) diff -= current_track_length;
                    
                    if (std::abs(diff) < 20.0) {
                        internal_meter += diff * 0.05; // 5% correction per tick
                    } else {
                        internal_meter = graphic_spline_pos; // hard reset on big jump
                    }
                }
                
                if (current_track_length > 0.0) {
                    while (internal_meter >= current_track_length) internal_meter -= current_track_length;
                    while (internal_meter < 0.0) internal_meter += current_track_length;
                }

                double spline_pos = internal_meter;
                double dist_diff = 0.0;
                
                if (last_recorded_meter >= 0.0) {
                    dist_diff = spline_pos - last_recorded_meter;
                    // fix distance jump when track spline loops
                    if (dist_diff < -current_track_length / 2.0) {
                        dist_diff += current_track_length;
                    } else if (dist_diff > current_track_length / 2.0) {
                        dist_diff -= current_track_length;
                    }
                }
                
                double current_meter = (last_recorded_meter < 0.0) ? spline_pos : (last_recorded_meter + dist_diff);
                double distance_threshold = 1.0 / samples_per_meter;

                // check if we moved enough to record a new sample
                if (last_recorded_meter < 0.0 || std::abs(current_meter - last_recorded_meter) >= distance_threshold) {
                    double old_last_recorded_meter = last_recorded_meter;
                    double direction = 1.0;
                    
                    if (last_recorded_meter < 0.0) {
                        last_recorded_meter = current_meter;
                    } else {
                        double jump_size = std::abs(current_meter - last_recorded_meter);
                        
                        // handle teleport jumps (session restart, pit return, etc..)
                        // if the distance jumped is too large in a single frame
                        // we don't try to interpolate all the way back (cuz that jump is impossible)
                        if (jump_size > 20.0 && current_track_length > 0.0 && 
                            jump_size < current_track_length - 20.0) {
                            last_recorded_meter = current_meter;
                        } else {
                            direction = (current_meter > last_recorded_meter) ? 1.0 : -1.0;
                            last_recorded_meter += distance_threshold * direction;
                            
                            if (current_track_length > 0.0) {
                                if (last_recorded_meter >= current_track_length) {
                                    last_recorded_meter -= current_track_length;
                                } else if (last_recorded_meter < 0.0) {
                                    last_recorded_meter += current_track_length;
                                }
                            }
                        }
                    }
                    
                    auto& lap = sessions_data.back();

                    // time interpolation pass
                    // since we trigger records based on high-frequency distance (complementary filter above),
                    // we must also interpolate the 60hz graphic time to match the exact moment of this sample.
                    // we use the 400hz physics packet difference to smoothly advance the clock.
                    double smoothed_timestamp = dataGraphic->iCurrentTime / 1000.0;
                    int32_t smoothed_iCurrentTime = dataGraphic->iCurrentTime;
                    double smoothed_normalizedCarPosition = dataGraphic->normalizedCarPosition;
                    float smoothed_distanceTraveled = dataGraphic->distanceTraveled;

                    if (current_track_length > 0.0) {
                        smoothed_normalizedCarPosition = last_recorded_meter / current_track_length;
                    }

                    if (!lap.timestamp.empty() && old_last_recorded_meter >= 0.0) {
                        int packet_diff = dataPhysics->packetId - lap.packetId_physics.back();
                        if (packet_diff > 0 && packet_diff < 1000) {
                            double dt = packet_diff * (1.0 / 400.0); // acc physics is 400hz
                            smoothed_timestamp = lap.timestamp.back() + dt;
                            smoothed_iCurrentTime = lap.iCurrentTime.back() + static_cast<int32_t>(dt * 1000.0);
                        }
                        smoothed_distanceTraveled = lap.distanceTraveled.back() + distance_threshold * direction;
                    }

                    // graphic basics
                    lap.timestamp.push_back(smoothed_timestamp);
                    lap.packetId_graphic.push_back(dataGraphic->packetId);
                    lap.iCurrentTime.push_back(smoothed_iCurrentTime);
                    lap.iLastTime.push_back(dataGraphic->iLastTime);
                    lap.iBestTime.push_back(dataGraphic->iBestTime);
                    lap.normalizedCarPosition.push_back(smoothed_normalizedCarPosition);
                    lap.distanceTraveled.push_back(smoothed_distanceTraveled);
                    lap.replayTimeMultiplier.push_back(dataGraphic->replayTimeMultiplier);
                    lap.numberOfLaps.push_back(dataGraphic->numberOfLaps);
                    lap.completedLaps.push_back(dataGraphic->completedLaps);
                    
                    lap.status.push_back(dataGraphic->status);
                    lap.session.push_back(dataGraphic->session);
                    lap.position.push_back(dataGraphic->position);
                    lap.sessionTimeLeft.push_back(dataGraphic->sessionTimeLeft);
                    lap.isInPit.push_back(dataGraphic->isInPit);
                    lap.currentSectorIndex.push_back(dataGraphic->currentSectorIndex);
                    lap.lastSectorTime.push_back(dataGraphic->lastSectorTime);
                    
                    // graphic strings
                    auto push_str = [](auto& target_vec, const wchar_t* src, size_t size) {
                        target_vec.emplace_back();
                        std::memcpy(target_vec.back().data(), src, size * sizeof(wchar_t));
                    };
                    push_str(lap.currentTime, dataGraphic->currentTime, 15);
                    push_str(lap.lastTime, dataGraphic->lastTime, 15);
                    push_str(lap.bestTime, dataGraphic->bestTime, 15);
                    push_str(lap.split, dataGraphic->split, 15);
                    push_str(lap.tyreCompound, dataGraphic->tyreCompound, 33);

                    // physics basics
                    lap.packetId_physics.push_back(dataPhysics->packetId);
                    lap.gas.push_back(dataPhysics->gas);
                    lap.brake.push_back(dataPhysics->brake);
                    lap.fuel.push_back(dataPhysics->fuel);
                    lap.gear.push_back(dataPhysics->gear);
                    lap.rpms.push_back(dataPhysics->rpms);
                    lap.steerAngle.push_back(dataPhysics->steerAngle);
                    lap.speedKmh.push_back(dataPhysics->speedKmh);
                    lap.isAIControlled.push_back(dataPhysics->isAIControlled);

                    // vectors
                    lap.velocity_x.push_back(dataPhysics->velocity[0]);
                    lap.velocity_y.push_back(dataPhysics->velocity[1]);
                    lap.velocity_z.push_back(dataPhysics->velocity[2]);
                    
                    lap.accG_x.push_back(dataPhysics->accG[0]);
                    lap.accG_y.push_back(dataPhysics->accG[1]);
                    lap.accG_z.push_back(dataPhysics->accG[2]);

                    // wheels
                    lap.wheelSlip_fl.push_back(dataPhysics->wheelSlip[0]);
                    lap.wheelSlip_fr.push_back(dataPhysics->wheelSlip[1]);
                    lap.wheelSlip_rl.push_back(dataPhysics->wheelSlip[2]);
                    lap.wheelSlip_rr.push_back(dataPhysics->wheelSlip[3]);

                    lap.wheelLoad_fl.push_back(dataPhysics->wheelLoad[0]);
                    lap.wheelLoad_fr.push_back(dataPhysics->wheelLoad[1]);
                    lap.wheelLoad_rl.push_back(dataPhysics->wheelLoad[2]);
                    lap.wheelLoad_rr.push_back(dataPhysics->wheelLoad[3]);

                    lap.wheelsPressure_fl.push_back(dataPhysics->wheelsPressure[0]);
                    lap.wheelsPressure_fr.push_back(dataPhysics->wheelsPressure[1]);
                    lap.wheelsPressure_rl.push_back(dataPhysics->wheelsPressure[2]);
                    lap.wheelsPressure_rr.push_back(dataPhysics->wheelsPressure[3]);

                    lap.wheelAngularSpeed_fl.push_back(dataPhysics->wheelAngularSpeed[0]);
                    lap.wheelAngularSpeed_fr.push_back(dataPhysics->wheelAngularSpeed[1]);
                    lap.wheelAngularSpeed_rl.push_back(dataPhysics->wheelAngularSpeed[2]);
                    lap.wheelAngularSpeed_rr.push_back(dataPhysics->wheelAngularSpeed[3]);

                    lap.tyreWear_fl.push_back(dataPhysics->tyreWear[0]);
                    lap.tyreWear_fr.push_back(dataPhysics->tyreWear[1]);
                    lap.tyreWear_rl.push_back(dataPhysics->tyreWear[2]);
                    lap.tyreWear_rr.push_back(dataPhysics->tyreWear[3]);

                    lap.tyreDirtyLevel_fl.push_back(dataPhysics->tyreDirtyLevel[0]);
                    lap.tyreDirtyLevel_fr.push_back(dataPhysics->tyreDirtyLevel[1]);
                    lap.tyreDirtyLevel_rl.push_back(dataPhysics->tyreDirtyLevel[2]);
                    lap.tyreDirtyLevel_rr.push_back(dataPhysics->tyreDirtyLevel[3]);

                    lap.tyreCoreTemperature_fl.push_back(dataPhysics->tyreCoreTemperature[0]);
                    lap.tyreCoreTemperature_fr.push_back(dataPhysics->tyreCoreTemperature[1]);
                    lap.tyreCoreTemperature_rl.push_back(dataPhysics->tyreCoreTemperature[2]);
                    lap.tyreCoreTemperature_rr.push_back(dataPhysics->tyreCoreTemperature[3]);

                    lap.camberRAD_fl.push_back(dataPhysics->camberRAD[0]);
                    lap.camberRAD_fr.push_back(dataPhysics->camberRAD[1]);
                    lap.camberRAD_rl.push_back(dataPhysics->camberRAD[2]);
                    lap.camberRAD_rr.push_back(dataPhysics->camberRAD[3]);

                    lap.suspensionTravel_fl.push_back(dataPhysics->suspensionTravel[0]);
                    lap.suspensionTravel_fr.push_back(dataPhysics->suspensionTravel[1]);
                    lap.suspensionTravel_rl.push_back(dataPhysics->suspensionTravel[2]);
                    lap.suspensionTravel_rr.push_back(dataPhysics->suspensionTravel[3]);

                    lap.brakeTemp_fl.push_back(dataPhysics->brakeTemp[0]);
                    lap.brakeTemp_fr.push_back(dataPhysics->brakeTemp[1]);
                    lap.brakeTemp_rl.push_back(dataPhysics->brakeTemp[2]);
                    lap.brakeTemp_rr.push_back(dataPhysics->brakeTemp[3]);

                    lap.tyreTempI_fl.push_back(dataPhysics->tyreTempI[0]);
                    lap.tyreTempI_fr.push_back(dataPhysics->tyreTempI[1]);
                    lap.tyreTempI_rl.push_back(dataPhysics->tyreTempI[2]);
                    lap.tyreTempI_rr.push_back(dataPhysics->tyreTempI[3]);

                    lap.tyreTempM_fl.push_back(dataPhysics->tyreTempM[0]);
                    lap.tyreTempM_fr.push_back(dataPhysics->tyreTempM[1]);
                    lap.tyreTempM_rl.push_back(dataPhysics->tyreTempM[2]);
                    lap.tyreTempM_rr.push_back(dataPhysics->tyreTempM[3]);

                    lap.tyreTempO_fl.push_back(dataPhysics->tyreTempO[0]);
                    lap.tyreTempO_fr.push_back(dataPhysics->tyreTempO[1]);
                    lap.tyreTempO_rl.push_back(dataPhysics->tyreTempO[2]);
                    lap.tyreTempO_rr.push_back(dataPhysics->tyreTempO[3]);

                    // tyre contact point
                    lap.tyreContactPoint_fl_x.push_back(dataPhysics->tyreContactPoint[0][0]);
                    lap.tyreContactPoint_fl_y.push_back(dataPhysics->tyreContactPoint[0][1]);
                    lap.tyreContactPoint_fl_z.push_back(dataPhysics->tyreContactPoint[0][2]);
                    lap.tyreContactPoint_fr_x.push_back(dataPhysics->tyreContactPoint[1][0]);
                    lap.tyreContactPoint_fr_y.push_back(dataPhysics->tyreContactPoint[1][1]);
                    lap.tyreContactPoint_fr_z.push_back(dataPhysics->tyreContactPoint[1][2]);
                    lap.tyreContactPoint_rl_x.push_back(dataPhysics->tyreContactPoint[2][0]);
                    lap.tyreContactPoint_rl_y.push_back(dataPhysics->tyreContactPoint[2][1]);
                    lap.tyreContactPoint_rl_z.push_back(dataPhysics->tyreContactPoint[2][2]);
                    lap.tyreContactPoint_rr_x.push_back(dataPhysics->tyreContactPoint[3][0]);
                    lap.tyreContactPoint_rr_y.push_back(dataPhysics->tyreContactPoint[3][1]);
                    lap.tyreContactPoint_rr_z.push_back(dataPhysics->tyreContactPoint[3][2]);

                    // tyre contact normal
                    lap.tyreContactNormal_fl_x.push_back(dataPhysics->tyreContactNormal[0][0]);
                    lap.tyreContactNormal_fl_y.push_back(dataPhysics->tyreContactNormal[0][1]);
                    lap.tyreContactNormal_fl_z.push_back(dataPhysics->tyreContactNormal[0][2]);
                    lap.tyreContactNormal_fr_x.push_back(dataPhysics->tyreContactNormal[1][0]);
                    lap.tyreContactNormal_fr_y.push_back(dataPhysics->tyreContactNormal[1][1]);
                    lap.tyreContactNormal_fr_z.push_back(dataPhysics->tyreContactNormal[1][2]);
                    lap.tyreContactNormal_rl_x.push_back(dataPhysics->tyreContactNormal[2][0]);
                    lap.tyreContactNormal_rl_y.push_back(dataPhysics->tyreContactNormal[2][1]);
                    lap.tyreContactNormal_rl_z.push_back(dataPhysics->tyreContactNormal[2][2]);
                    lap.tyreContactNormal_rr_x.push_back(dataPhysics->tyreContactNormal[3][0]);
                    lap.tyreContactNormal_rr_y.push_back(dataPhysics->tyreContactNormal[3][1]);
                    lap.tyreContactNormal_rr_z.push_back(dataPhysics->tyreContactNormal[3][2]);

                    // tyre contact heading
                    lap.tyreContactHeading_fl_x.push_back(dataPhysics->tyreContactHeading[0][0]);
                    lap.tyreContactHeading_fl_y.push_back(dataPhysics->tyreContactHeading[0][1]);
                    lap.tyreContactHeading_fl_z.push_back(dataPhysics->tyreContactHeading[0][2]);
                    lap.tyreContactHeading_fr_x.push_back(dataPhysics->tyreContactHeading[1][0]);
                    lap.tyreContactHeading_fr_y.push_back(dataPhysics->tyreContactHeading[1][1]);
                    lap.tyreContactHeading_fr_z.push_back(dataPhysics->tyreContactHeading[1][2]);
                    lap.tyreContactHeading_rl_x.push_back(dataPhysics->tyreContactHeading[2][0]);
                    lap.tyreContactHeading_rl_y.push_back(dataPhysics->tyreContactHeading[2][1]);
                    lap.tyreContactHeading_rl_z.push_back(dataPhysics->tyreContactHeading[2][2]);
                    lap.tyreContactHeading_rr_x.push_back(dataPhysics->tyreContactHeading[3][0]);
                    lap.tyreContactHeading_rr_y.push_back(dataPhysics->tyreContactHeading[3][1]);
                    lap.tyreContactHeading_rr_z.push_back(dataPhysics->tyreContactHeading[3][2]);

                    // physics advanced
                    lap.drs.push_back(dataPhysics->drs);
                    lap.tc.push_back(dataPhysics->tc);
                    lap.heading.push_back(dataPhysics->heading);
                    lap.pitch.push_back(dataPhysics->pitch);
                    lap.roll.push_back(dataPhysics->roll);
                    lap.cgHeight.push_back(dataPhysics->cgHeight);
                    lap.pitLimiterOn.push_back(dataPhysics->pitLimiterOn);
                    lap.abs.push_back(dataPhysics->abs);
                    lap.kersCharge.push_back(dataPhysics->kersCharge);
                    lap.kersInput.push_back(dataPhysics->kersInput);
                    lap.autoShifterOn.push_back(dataPhysics->autoShifterOn);
                    lap.rideHeight_f.push_back(dataPhysics->rideHeight[0]);
                    lap.rideHeight_r.push_back(dataPhysics->rideHeight[1]);
                    lap.turboBoost.push_back(dataPhysics->turboBoost);
                    lap.ballast.push_back(dataPhysics->ballast);
                    lap.airDensity.push_back(dataPhysics->airDensity);
                    lap.airTemp.push_back(dataPhysics->airTemp);
                    lap.roadTemp.push_back(dataPhysics->roadTemp);
                    lap.localAngularVel_x.push_back(dataPhysics->localAngularVel[0]);
                    lap.localAngularVel_y.push_back(dataPhysics->localAngularVel[1]);
                    lap.localAngularVel_z.push_back(dataPhysics->localAngularVel[2]);
                    lap.finalFF.push_back(dataPhysics->finalFF);
                    lap.performanceMeter.push_back(dataPhysics->performanceMeter);
                    lap.engineBrake.push_back(dataPhysics->engineBrake);
                    lap.ersRecoveryLevel.push_back(dataPhysics->ersRecoveryLevel);
                    lap.ersPowerLevel.push_back(dataPhysics->ersPowerLevel);
                    lap.ersHeatCharging.push_back(dataPhysics->ersHeatCharging);
                    lap.ersIsCharging.push_back(dataPhysics->ersIsCharging);
                    lap.kersCurrentKJ.push_back(dataPhysics->kersCurrentKJ);
                    lap.drsAvailable.push_back(dataPhysics->drsAvailable);
                    lap.drsEnabled.push_back(dataPhysics->drsEnabled);
                    lap.clutch.push_back(dataPhysics->clutch);
                    lap.brakeBias.push_back(dataPhysics->brakeBias);
                    lap.localVelocity_x.push_back(dataPhysics->localVelocity[0]);
                    lap.localVelocity_y.push_back(dataPhysics->localVelocity[1]);
                    lap.localVelocity_z.push_back(dataPhysics->localVelocity[2]);

                    lap.carDamage_0.push_back(dataPhysics->carDamage[0]);
                    lap.carDamage_1.push_back(dataPhysics->carDamage[1]);
                    lap.carDamage_2.push_back(dataPhysics->carDamage[2]);
                    lap.carDamage_3.push_back(dataPhysics->carDamage[3]);
                    lap.carDamage_4.push_back(dataPhysics->carDamage[4]);
                    lap.numberOfTyresOut.push_back(dataPhysics->numberOfTyresOut);

                    // acc physics
                    lap.mz_FL.push_back(dataPhysics->mz[0]);
                    lap.mz_FR.push_back(dataPhysics->mz[1]);
                    lap.mz_RL.push_back(dataPhysics->mz[2]);
                    lap.mz_RR.push_back(dataPhysics->mz[3]);

                    lap.fx_FL.push_back(dataPhysics->fx[0]);
                    lap.fx_FR.push_back(dataPhysics->fx[1]);
                    lap.fx_RL.push_back(dataPhysics->fx[2]);
                    lap.fx_RR.push_back(dataPhysics->fx[3]);

                    lap.fy_FL.push_back(dataPhysics->fy[0]);
                    lap.fy_FR.push_back(dataPhysics->fy[1]);
                    lap.fy_RL.push_back(dataPhysics->fy[2]);
                    lap.fy_RR.push_back(dataPhysics->fy[3]);

                    lap.slipRatio_FL.push_back(dataPhysics->slipRatio[0]);
                    lap.slipRatio_FR.push_back(dataPhysics->slipRatio[1]);
                    lap.slipRatio_RL.push_back(dataPhysics->slipRatio[2]);
                    lap.slipRatio_RR.push_back(dataPhysics->slipRatio[3]);

                    lap.slipAngle_FL.push_back(dataPhysics->slipAngle[0]);
                    lap.slipAngle_FR.push_back(dataPhysics->slipAngle[1]);
                    lap.slipAngle_RL.push_back(dataPhysics->slipAngle[2]);
                    lap.slipAngle_RR.push_back(dataPhysics->slipAngle[3]);

                    lap.tcinAction.push_back(dataPhysics->tcinAction);
                    lap.absInAction.push_back(dataPhysics->absInAction);

                    lap.suspensionDamage_FL.push_back(dataPhysics->suspensionDamage[0]);
                    lap.suspensionDamage_FR.push_back(dataPhysics->suspensionDamage[1]);
                    lap.suspensionDamage_RL.push_back(dataPhysics->suspensionDamage[2]);
                    lap.suspensionDamage_RR.push_back(dataPhysics->suspensionDamage[3]);

                    lap.tyreTemp_FL.push_back(dataPhysics->tyreTemp[0]);
                    lap.tyreTemp_FR.push_back(dataPhysics->tyreTemp[1]);
                    lap.tyreTemp_RL.push_back(dataPhysics->tyreTemp[2]);
                    lap.tyreTemp_RR.push_back(dataPhysics->tyreTemp[3]);

                    lap.waterTemp.push_back(dataPhysics->waterTemp);

                    lap.brakePressure_FL.push_back(dataPhysics->brakePressure[0]);
                    lap.brakePressure_FR.push_back(dataPhysics->brakePressure[1]);
                    lap.brakePressure_RL.push_back(dataPhysics->brakePressure[2]);
                    lap.brakePressure_RR.push_back(dataPhysics->brakePressure[3]);

                    lap.padLife_FL.push_back(dataPhysics->padLife[0]);
                    lap.padLife_FR.push_back(dataPhysics->padLife[1]);
                    lap.padLife_RL.push_back(dataPhysics->padLife[2]);
                    lap.padLife_RR.push_back(dataPhysics->padLife[3]);

                    lap.discLife_FL.push_back(dataPhysics->discLife[0]);
                    lap.discLife_FR.push_back(dataPhysics->discLife[1]);
                    lap.discLife_RL.push_back(dataPhysics->discLife[2]);
                    lap.discLife_RR.push_back(dataPhysics->discLife[3]);

                    lap.kerbVibration.push_back(dataPhysics->kerbVibration);
                    lap.slipVibrations.push_back(dataPhysics->slipVibrations);
                    lap.gVibrations.push_back(dataPhysics->gVibrations);
                    lap.absVibrations.push_back(dataPhysics->absVibrations);

                    // graphic
                    lap.status.push_back(dataGraphic->status);
                    lap.session.push_back(dataGraphic->session);
                    lap.position.push_back(dataGraphic->position);
                    lap.sessionTimeLeft.push_back(dataGraphic->sessionTimeLeft);
                    lap.isInPit.push_back(dataGraphic->isInPit);
                    lap.currentSectorIndex.push_back(dataGraphic->currentSectorIndex);
                    lap.lastSectorTime.push_back(dataGraphic->lastSectorTime);

                    // dynamic car coordinate tracking
                    // acc assigns unique connection ids (e.g. 1046) that easily exceed the 60 slot map limit.
                    // to find our exact coordinate index without slowing down the 400Hz tick, we use a 3-step search:
                    int player_id = dataGraphic->playerCarID;
                    int found_index = 0;
                    
                    if (player_id >= 0 && player_id < 60 && dataGraphic->carID[player_id] == player_id) {
                        // step 1 (offline/fast path): if id is small, it usually matches the index perfectly
                        found_index = player_id;
                    } else if (player_id >= 0) {
                        if (last_player_car_index >= 0 && last_player_car_index < 60 && dataGraphic->carID[last_player_car_index] == player_id) {
                            // step 2 (cache path): check if we are still at the same index as the last tick.
                            // this hits 99.9% of the time, resulting in zero loop overhead.
                            found_index = last_player_car_index;
                        } else {
                            // step 3 (fallback path): only runs when we first join or if the server shuffles the array.
                            // we scan all 60 slots to find our id, and cache the index for the next tick.
                            for (int i = 0; i < 60; ++i) {
                                if (dataGraphic->carID[i] == player_id) {
                                    found_index = i;
                                    last_player_car_index = i;
                                    break;
                                }
                            }
                        }
                    }

                    lap.carCoordinates_x.push_back(dataGraphic->carCoordinates[found_index][0]);
                    lap.carCoordinates_y.push_back(dataGraphic->carCoordinates[found_index][1]);
                    lap.carCoordinates_z.push_back(dataGraphic->carCoordinates[found_index][2]);

                    std::array<std::array<float, 3>, 60> coords_arr;
                    std::memcpy(coords_arr.data(), dataGraphic->carCoordinates, sizeof(coords_arr));
                    lap.carCoordinates.push_back(coords_arr);

                    std::array<int, 60> car_id_arr;
                    std::memcpy(car_id_arr.data(), dataGraphic->carID, sizeof(car_id_arr));
                    lap.carID.push_back(car_id_arr);

                    lap.activeCars.push_back(dataGraphic->activeCars);
                    lap.playerCarID.push_back(dataGraphic->playerCarID);
                    lap.penaltyTime.push_back(dataGraphic->penaltyTime);
                    lap.flag.push_back(dataGraphic->flag);
                    lap.penalty.push_back(dataGraphic->penalty);
                    lap.idealLineOn.push_back(dataGraphic->idealLineOn);
                    lap.isInPitLane.push_back(dataGraphic->isInPitLane);
                    lap.surfaceGrip.push_back(dataGraphic->surfaceGrip);
                    lap.mandatoryPitDone.push_back(dataGraphic->mandatoryPitDone);
                    lap.windSpeed.push_back(dataGraphic->windSpeed);
                    lap.windDirection.push_back(dataGraphic->windDirection);

                    lap.TC.push_back(dataGraphic->TC);
                    lap.TCCUT.push_back(dataGraphic->TCCUT);
                    lap.EngineMap.push_back(dataGraphic->EngineMap);
                    lap.ABS.push_back(dataGraphic->ABS);
                    lap.exhaustTemperature.push_back(dataGraphic->exhaustTemperature);

                    lap.isSetupMenuVisible.push_back(dataGraphic->isSetupMenuVisible);
                    lap.mainDisplayIndex.push_back(dataGraphic->mainDisplayIndex);
                    lap.secondaryDisplyIndex.push_back(dataGraphic->secondaryDisplyIndex);
                    lap.fuelXLap.push_back(dataGraphic->fuelXLap);
                    lap.rainLights.push_back(dataGraphic->rainLights);
                    lap.flashingLights.push_back(dataGraphic->flashingLights);
                    lap.lightsStage.push_back(dataGraphic->lightsStage);
                    lap.wiperLV.push_back(dataGraphic->wiperLV);
                    lap.driverStintTotalTimeLeft.push_back(dataGraphic->driverStintTotalTimeLeft);
                    lap.driverStintTimeLeft.push_back(dataGraphic->driverStintTimeLeft);
                    lap.rainTyres.push_back(dataGraphic->rainTyres);
                    lap.sessionIndex.push_back(dataGraphic->sessionIndex);
                    lap.usedFuel.push_back(dataGraphic->usedFuel);

                    push_str(lap.deltaLapTime, dataGraphic->deltaLapTime, 15);
                    lap.iDeltaLapTime.push_back(dataGraphic->iDeltaLapTime);
                    push_str(lap.estimatedLapTime, dataGraphic->estimatedLapTime, 15);
                    lap.iEstimatedLapTime.push_back(dataGraphic->iEstimatedLapTime);
                    lap.isDeltaPositive.push_back(dataGraphic->isDeltaPositive);
                    lap.iSplit.push_back(dataGraphic->iSplit);
                    lap.isValidLap.push_back(dataGraphic->isValidLap);
                    lap.fuelEstimatedLaps.push_back(dataGraphic->fuelEstimatedLaps);
                    push_str(lap.trackStatus, dataGraphic->trackStatus, 33);
                    lap.missingMandatoryPits.push_back(dataGraphic->missingMandatoryPits);
                    lap.Clock.push_back(dataGraphic->Clock);
                    lap.directionLightsLeft.push_back(dataGraphic->directionLightsLeft);
                    lap.directionLightsRight.push_back(dataGraphic->directionLightsRight);
                    lap.GlobalYellow.push_back(dataGraphic->GlobalYellow);
                    lap.GlobalYellow1.push_back(dataGraphic->GlobalYellow1);
                    lap.GlobalYellow2.push_back(dataGraphic->GlobalYellow2);
                    lap.GlobalYellow3.push_back(dataGraphic->GlobalYellow3);
                    lap.GlobalWhite.push_back(dataGraphic->GlobalWhite);
                    lap.GlobalGreen.push_back(dataGraphic->GlobalGreen);
                    lap.GlobalChequered.push_back(dataGraphic->GlobalChequered);
                    lap.GlobalRed.push_back(dataGraphic->GlobalRed);
                    lap.mfdTyreSet.push_back(dataGraphic->mfdTyreSet);
                    lap.mfdFuelToAdd.push_back(dataGraphic->mfdFuelToAdd);
                    lap.mfdTyrePressureLF.push_back(dataGraphic->mfdTyrePressureLF);
                    lap.mfdTyrePressureRF.push_back(dataGraphic->mfdTyrePressureRF);
                    lap.mfdTyrePressureLR.push_back(dataGraphic->mfdTyrePressureLR);
                    lap.mfdTyrePressureRR.push_back(dataGraphic->mfdTyrePressureRR);
                    lap.trackGripStatus.push_back(dataGraphic->trackGripStatus);
                    lap.rainIntensity.push_back(dataGraphic->rainIntensity);
                    lap.rainIntensityIn10min.push_back(dataGraphic->rainIntensityIn10min);
                    lap.rainIntensityIn30min.push_back(dataGraphic->rainIntensityIn30min);
                    lap.currentTyreSet.push_back(dataGraphic->currentTyreSet);
                    lap.strategyTyreSet.push_back(dataGraphic->strategyTyreSet);
                    lap.gapAhead.push_back(dataGraphic->gapAhead);
                    lap.gapBehind.push_back(dataGraphic->gapBehind);
                }
            }
        }

        std::this_thread::sleep_until(next_tick);
    }

    timeEndPeriod(1);
}

#pragma comment(lib, "winmm.lib")

String ACCProvider::load_session(const String& file_path) {
    loaded_session_data.clear();
    loaded_session_lap_offsets.clear();

    TelemetryFile infile;
    uint64_t count = 0;
    int64_t dummy_ts;
    String err = _open_session_file(file_path, infile, loaded_session_static_data, loaded_session_sample_interval, loaded_session_samples_per_meter, count, loaded_session_lap_offsets, dummy_ts);
    if (!err.is_empty()) return err;

    loaded_session_lap_count = count;

    for (int i = 0; i < loaded_session_lap_count; ++i) {
        ACC_LapDataChannels lap_data;
        infile.seekg(loaded_session_lap_offsets[i]);
        lap_data.read_from_stream(infile);
        if (infile.fail()) {
            return String("failed to read snapshot data for lap ") + String::num_int64(i);
        }
        
        if (lap_data.speedKmh.empty()) continue;
        
        // compatibility for old telemetry files (before commit 'e35eb69')
        if (!lap_data.normalizedCarPosition.empty()) {
            float base_offset = 0.0f;
            float prev_pos = lap_data.normalizedCarPosition[0];
            for (size_t j = 1; j < lap_data.normalizedCarPosition.size(); ++j) {
                float curr_pos = lap_data.normalizedCarPosition[j];
                if (curr_pos - prev_pos < -0.5f) {
                    base_offset += 1.0f;
                } else if (curr_pos - prev_pos > 0.5f) {
                    base_offset -= 1.0f;
                }
                prev_pos = curr_pos;
                lap_data.normalizedCarPosition[j] = curr_pos + base_offset;
            }
        }
        
        loaded_session_data.push_back(lap_data);
    }

    infile.close();
    return "";
}

Dictionary ACCProvider::get_live_static_data() {
    if (!is_connected_flag || !dataStatic) return Dictionary();
    return _static_to_dict(*dataStatic);
}

Dictionary ACCProvider::get_loaded_session_static_data() {
    return _static_to_dict(loaded_session_static_data);
}

double ACCProvider::get_loaded_session_lap_fuel_consumption(int lap_index) {
    if (lap_index < 0 || lap_index >= loaded_session_data.size()) return 0.0;
    const auto &lap = loaded_session_data[lap_index];
    if (lap.fuel.empty()) return 0.0;

    double consumed = 0.0;
    float prev_fuel = lap.fuel[0];
    for (size_t i = 1; i < lap.fuel.size(); i++) {
        float current_fuel = lap.fuel[i];
        if (current_fuel < prev_fuel) {
            consumed += (prev_fuel - current_fuel);
        }
        prev_fuel = current_fuel;
    }
    return consumed;
}

double ACCProvider::get_loaded_session_total_fuel_consumption() {
    double total = 0.0;
    for (int i = 0; i < loaded_session_data.size(); i++) {
        total += get_loaded_session_lap_fuel_consumption(i);
    }
    return total;
}

double ACCProvider::get_loaded_session_total_laps() {
    double total_driven_fraction = 0.0;
    bool has_prev = false;
    double prev_pos = 0.0;
    
    for (size_t i = 0; i < loaded_session_data.size(); i++) {
        const auto &lap = loaded_session_data[i];
        if (lap.normalizedCarPosition.empty()) continue;
        
        for (size_t j = 0; j < lap.normalizedCarPosition.size(); j++) {
            double curr_pos = lap.normalizedCarPosition[j];
            
            if (has_prev) {
                double diff = curr_pos - prev_pos;
                
                if (diff < -0.5 && prev_pos > 0.8 && curr_pos < 0.2) {
                    diff += 1.0;
                } else if (diff > 0.5 && prev_pos < 0.2 && curr_pos > 0.8) {
                    diff -= 1.0;
                }
                
                if (std::abs(diff) > 0.1) {
                    diff = 0.0; // ignore large jumps
                }
                
                total_driven_fraction += diff;
            }
            
            prev_pos = curr_pos;
            has_prev = true;
        }
    }
    
    return total_driven_fraction;
}

Dictionary ACCProvider::get_loaded_session_lap_stats(int lap_index) {
    Dictionary stats;
    if (lap_index < 0 || lap_index >= loaded_session_data.size()) return stats;

    const auto &lap = loaded_session_data[lap_index];
    if (lap.timestamp.empty()) return stats;

    int lap_time = 0;
    Dictionary sector_times;
    float top_speed = 0.0f;
    int current_sec_idx = lap.currentSectorIndex[0];

    for (size_t i = 0; i < lap.timestamp.size(); i++) {
        // detect sector change
        if (lap.currentSectorIndex[i] != current_sec_idx) {
            if (lap.lastSectorTime[i] > 0 && lap.lastSectorTime[i] <= lap.iCurrentTime[i] + 2000) {
                sector_times[current_sec_idx] = lap.lastSectorTime[i];
            }
            current_sec_idx = lap.currentSectorIndex[i];
        }

        if (lap.speedKmh[i] > top_speed) {
            top_speed = lap.speedKmh[i];
        }
    }

    bool is_completed = false;
    if (lap.normalizedCarPosition.size() > 1) {
        float total_progression = 0.0f;
        for (size_t k = 1; k < lap.normalizedCarPosition.size(); k++) {
            float diff = lap.normalizedCarPosition[k] - lap.normalizedCarPosition[k-1];
            if (diff < -0.5f) diff += 1.0f;
            if (diff > 0.5f) diff -= 1.0f;
            
            if (diff > 0.0f && diff < 0.1f) {
                total_progression += diff;
            }
        }
        if (total_progression > 0.90f) {
            is_completed = true;
        }
    }

    // try to get the exact lap_time and final sector time from the next lap
    if (is_completed && lap_index + 1 < loaded_session_data.size() && !loaded_session_data[lap_index + 1].timestamp.empty()) {
        const auto &next_lap = loaded_session_data[lap_index + 1];
        
        lap_time = 0;
        for (int t : next_lap.iLastTime) {
            if (t > lap_time) lap_time = t;
        }
        if (lap_time == 0 && !lap.iCurrentTime.empty()) {
            lap_time = lap.iCurrentTime.back();
        }
        
        for (size_t k = 0; k < next_lap.currentSectorIndex.size(); k++) {
            if (next_lap.currentSectorIndex[k] == 0 && next_lap.lastSectorTime[k] > 0) {
                sector_times[current_sec_idx] = next_lap.lastSectorTime[k];
                break;
            }
        }
    } else {
        lap_time = lap.iCurrentTime.empty() ? 0 : lap.iCurrentTime.back();
    }

    stats["lap_time_ms"] = lap_time;
    stats["sector_times_ms"] = sector_times;
    stats["top_speed_kmh"] = top_speed;
    stats["snapshot_count"] = (int)lap.timestamp.size();
    stats["is_completed"] = is_completed;
    
    return stats;
}

Dictionary ACCProvider::calculate_lap_time_delta(const String& target_file_path, int target_lap_index, const String& current_file_path, int current_lap_index, const PackedFloat32Array& reference_positions) {
    PackedFloat32Array cumulative_delta;
    Dictionary result;
    result["cumulative_delta"] = PackedFloat32Array();
    result["delta_rate"] = PackedFloat32Array();

    if (target_file_path.is_empty() || target_lap_index < 0 || current_lap_index < 0) return result;
    
    String actual_curr_path = current_file_path.is_empty() ? target_file_path : current_file_path;

    double session_spm = 1.0;

    auto load_lap = [this, &session_spm](String file_path, int lap_index, ACC_LapDataChannels &out_lap) -> bool {
        TelemetryFile infile;
        ACC_SPageStatic stat;
        double interval, spm;
        uint64_t count;
        std::vector<uint64_t> offsets;

        int64_t timestamp = 0;
        if (!_open_session_file(file_path, infile, stat, interval, spm, count, offsets, timestamp).is_empty()) {
            return false;
        }

        if (lap_index < 0 || lap_index >= count) return false;

        session_spm = spm;

        infile.seekg(offsets[lap_index]);
        out_lap.read_from_stream(infile);
        return true;
    };

    ACC_LapDataChannels target_lap, current_lap;
    if (!load_lap(target_file_path, target_lap_index, target_lap)) return result;
    
    if (actual_curr_path == target_file_path && current_lap_index == target_lap_index) {
        current_lap = target_lap;
    } else {
        if (!load_lap(actual_curr_path, current_lap_index, current_lap)) return result;
    }

    const std::vector<float>& raw_target_pos = target_lap.normalizedCarPosition;
    const std::vector<double>& target_time = target_lap.timestamp;
    
    const std::vector<float>& raw_current_pos = current_lap.normalizedCarPosition;
    const std::vector<double>& current_time = current_lap.timestamp;

    int target_n = raw_target_pos.size();
    int current_n = raw_current_pos.size();
    if (target_n < 2 || current_n < 2) return result;

    const float* ref_data = nullptr;
    int ref_n = 0;
    if (reference_positions.size() > 0) {
        ref_data = reference_positions.ptr();
        ref_n = reference_positions.size();
    } else {
        ref_data = raw_current_pos.data();
        ref_n = raw_current_pos.size();
    }

    if (ref_n == 0) return result;

    cumulative_delta.resize(ref_n);
    float* ptr = cumulative_delta.ptrw();

    // handle wrap-around at the end of the lap for target
    std::vector<float> target_pos(target_n);
    float last_target_raw_p = raw_target_pos[0];
    float target_p_offset = 0.0f;
    for (int i = 0; i < target_n; ++i) {
        float raw_p = raw_target_pos[i];
        if (i > 0 && raw_p < last_target_raw_p - 0.5f) {
            target_p_offset += 1.0f;
        }
        last_target_raw_p = raw_p;
        target_pos[i] = raw_p + target_p_offset;
    }

    // handle wrap-around for current
    std::vector<float> curr_pos(current_n);
    float last_curr_raw_p = raw_current_pos[0];
    float curr_p_offset = 0.0f;
    for (int i = 0; i < current_n; ++i) {
        float raw_p = raw_current_pos[i];
        if (i > 0 && raw_p < last_curr_raw_p - 0.5f) {
            curr_p_offset += 1.0f;
        }
        last_curr_raw_p = raw_p;
        curr_pos[i] = raw_p + curr_p_offset;
    }

    int target_idx = 0;
    int curr_idx = 0;

    float last_ref_raw_p = ref_data[0];
    float ref_p_offset = 0.0f;

    for (int i = 0; i < ref_n; ++i) {
        float raw_p = ref_data[i];
        
        if (i > 0 && raw_p < last_ref_raw_p - 0.5f) {
            ref_p_offset += 1.0f;
        }
        last_ref_raw_p = raw_p;
        float ref_p = raw_p + ref_p_offset;

        // interpolate target
        while (target_idx < target_n - 1 && target_pos[target_idx + 1] < ref_p) {
            target_idx++;
        }

        float t_time;
        bool t_valid = true;
        if (target_idx >= target_n - 1) {
            if (ref_p > target_pos[target_n - 1]) {
                t_valid = false;
            } else {
                t_time = (float)target_time[target_n - 1];
            }
        } else if (ref_p < target_pos[0]) {
            t_valid = false;
        } else {
            float p1 = target_pos[target_idx];
            float p2 = target_pos[target_idx + 1];
            float t1 = (float)target_time[target_idx];
            float t2 = (float)target_time[target_idx + 1];
            
            if (p2 - p1 > 0.000001f) {
                float t = (ref_p - p1) / (p2 - p1);
                t_time = t1 + t * (t2 - t1);
            } else {
                t_time = t1;
            }
        }

        // interpolate current
        while (curr_idx < current_n - 1 && curr_pos[curr_idx + 1] < ref_p) {
            curr_idx++;
        }

        float c_time;
        bool c_valid = true;
        if (curr_idx >= current_n - 1) {
            if (ref_p > curr_pos[current_n - 1]) {
                c_valid = false;
            } else {
                c_time = (float)current_time[current_n - 1];
            }
        } else if (ref_p < curr_pos[0]) {
            c_valid = false;
        } else {
            float p1 = curr_pos[curr_idx];
            float p2 = curr_pos[curr_idx + 1];
            float t1 = (float)current_time[curr_idx];
            float t2 = (float)current_time[curr_idx + 1];
            
            if (p2 - p1 > 0.000001f) {
                float t = (ref_p - p1) / (p2 - p1);
                c_time = t1 + t * (t2 - t1);
            } else {
                c_time = t1;
            }
        }
        
        if (!t_valid || !c_valid) {
            ptr[i] = std::numeric_limits<float>::quiet_NaN();
        } else {
            ptr[i] = c_time - t_time;
        }
    }

    // calculated math channels usually suffer from high-frequency noise
    // due to position quantization and interpolation limits.
    // we apply a centered moving average to smooth the delta without introducing phase shift.
    // dynamic window size targeting 25 meters of physical track distance.
    int window_size = std::max(3, (int)(25.0 * session_spm));
    if (window_size % 2 == 0) window_size += 1; // ensure odd number
    smooth_float_array(cumulative_delta, window_size);

    result["cumulative_delta"] = cumulative_delta;

    PackedFloat32Array delta_rate;
    delta_rate.resize(cumulative_delta.size());
    float* delta_rate_ptr = delta_rate.ptrw();
    for (int i = 1; i < cumulative_delta.size(); i++) {
        delta_rate_ptr[i] = cumulative_delta[i] - cumulative_delta[i - 1];
    }
    result["delta_rate"] = delta_rate;

    return result;
}
