#include "ac_provider.h"
#include "helper.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <cmath>
#include <algorithm>
#include <climits>
#include <windows.h>
#include <map>

using namespace godot;

ACProvider::ACProvider() {
    hMapPhysics = NULL;
    hMapGraphic = NULL;
    hMapStatic = NULL;
    dataPhysics = nullptr;
    dataGraphic = nullptr;
    dataStatic = nullptr;
}

ACProvider::~ACProvider() {
    is_logging = false;
    if (logging_thread.joinable()) {
        logging_thread.join();
    }
    disconnect_provider();
}

bool ACProvider::check_is_active() {
    HANDLE hMapStatic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (hMapStatic) {
        void* map_data = MapViewOfFile(hMapStatic, FILE_MAP_READ, 0, 0, 60);
        if (map_data) {
            String smVersion = wchar_to_gdstring((const wchar_t*)map_data, 15);
            UnmapViewOfFile(map_data);
            CloseHandle(hMapStatic);
            if (!smVersion.begins_with("1.7")) return false; // can be improved
            
            HANDLE hMapGraphic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
            if (hMapGraphic) {
                void* graphic_data = MapViewOfFile(hMapGraphic, FILE_MAP_READ, 0, 0, sizeof(AC_SPageGraphic));
                if (graphic_data) {
                    AC_STATUS status = ((AC_SPageGraphic*)graphic_data)->status;
                    UnmapViewOfFile(graphic_data);
                    CloseHandle(hMapGraphic);
                    return status != AC_OFF;
                }
                CloseHandle(hMapGraphic);
            }
            return false;
        }
        CloseHandle(hMapStatic);
    }
    return false;
}

String ACProvider::connect_provider() {
    is_connected_flag = false;

    if (dataPhysics) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; }
    if (dataGraphic) { UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; }
    if (hMapPhysics) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; }
    if (hMapGraphic) { CloseHandle(hMapGraphic); hMapGraphic = nullptr; }

    hMapPhysics = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
    if (hMapPhysics == NULL) return "Physics Map Error";

    dataPhysics = (AC_SPagePhysics*)MapViewOfFile(hMapPhysics, FILE_MAP_READ, 0, 0, sizeof(AC_SPagePhysics));
    if (dataPhysics == nullptr) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; return "Physics MapViewOfFile failed"; }

    hMapGraphic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
    if (hMapGraphic == NULL) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr; return "Graphic Map Error"; }
    
    dataGraphic = (AC_SPageGraphic*)MapViewOfFile(hMapGraphic, FILE_MAP_READ, 0, 0, sizeof(AC_SPageGraphic));
    if (dataGraphic == nullptr) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        CloseHandle(hMapGraphic); hMapGraphic = nullptr; return "Graphic MapViewOfFile failed";
    }

    hMapStatic = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (hMapStatic == NULL) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; CloseHandle(hMapGraphic); hMapGraphic = nullptr; return "Static Map Error";
    }
    
    dataStatic = (AC_SPageStatic*)MapViewOfFile(hMapStatic, FILE_MAP_READ, 0, 0, sizeof(AC_SPageStatic));
    if (dataStatic == nullptr) {
        UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; CloseHandle(hMapPhysics); hMapPhysics = nullptr;
        UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; CloseHandle(hMapGraphic); hMapGraphic = nullptr;
        CloseHandle(hMapStatic); hMapStatic = nullptr; return "Static MapViewOfFile failed";
    }

    is_connected_flag = true;
    return "";
}

void ACProvider::disconnect_provider() {
    stop_capture();
    is_connected_flag = false;
    if (dataPhysics) { UnmapViewOfFile(dataPhysics); dataPhysics = nullptr; }
    if (dataGraphic) { UnmapViewOfFile(dataGraphic); dataGraphic = nullptr; }
    if (dataStatic) { UnmapViewOfFile(dataStatic); dataStatic = nullptr; }
    if (hMapPhysics) { CloseHandle(hMapPhysics); hMapPhysics = nullptr; }
    if (hMapGraphic) { CloseHandle(hMapGraphic); hMapGraphic = nullptr; }
    if (hMapStatic) { CloseHandle(hMapStatic); hMapStatic = nullptr; }
}

void ACProvider::update() {
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

void ACProvider::set_auto_save_callback(std::function<void(godot::String)> callback) {
    auto_save_callback = callback;
}

String ACProvider::start_capture(const String& output_file_path) {
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

    logging_thread = std::thread(&ACProvider::logging_loop, this);
    return "";
}

String ACProvider::stop_capture(const String& output_file_path) {
    if (!is_connected_flag) return "AC is not connected.";
    if (!is_logging) return "Telemetry is not working.";

    is_logging = false;
    if (logging_thread.joinable()) {
        logging_thread.join();
    }
    
    std::lock_guard<std::mutex> lock(data_mutex);

    // remove empty/junk laps
    for (auto it = sessions_data.begin(); it != sessions_data.end(); ) {
        if (it->speedKmh.empty() || it->iCurrentTime.empty() || it->timestamp.size() < 10) {
            it = sessions_data.erase(it);
        } else {
            ++it;
        }
    }
    
    if (sessions_data.empty()) return "No valid telemetry data was recorded.";

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

    std::vector<AC_LapDataChannels> data_to_save = std::move(sessions_data);
    sessions_data.clear();
    
    AC_SPageStatic static_copy = *dataStatic;

    double s_spm = samples_per_meter;
    std::thread([this, data_to_save = std::move(data_to_save), static_copy, s_spm, final_path]() mutable {
        this->_flush_sessions_to_disk(std::move(data_to_save), static_copy, s_spm, final_path);
    }).detach();

    return final_path;
}

String ACProvider::_get_session_suffix(int session_enum) {
    switch (session_enum) {
        case AC_UNKNOWN: return "";
        case AC_PRACTICE: return ".prac";
        case AC_QUALIFY: return ".quali";
        case AC_RACE: return ".race";
        case AC_HOTLAP: return ".hl";
        case AC_TIME_ATTACK: return ".ta";
        case AC_DRIFT: return ".drift";
        case AC_DRAG: return ".drag";
        default: return "";
    }
}

void ACProvider::_flush_sessions_to_disk(std::vector<AC_LapDataChannels> data_to_save, AC_SPageStatic static_data_copy, double save_spm, String path) {
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
    
    if (data_to_save.empty()) return;

    String os_path = path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = ProjectSettings::get_singleton()->globalize_path(os_path);
    }

    CharString cs = os_path.utf8();
    std::string std_path(cs.get_data(), cs.length());

    TelemetryFile outfile;
    if (!outfile.open_write()) return;

    CharString utf8_signature = save_file_signature.utf8();
    outfile.write(utf8_signature.get_data(), utf8_signature.length());
    if (outfile.fail()) { outfile.close(); return; }

    int64_t session_timestamp = Time::get_singleton()->get_unix_time_from_system();
    outfile.write(&session_timestamp, sizeof(int64_t));
    if (outfile.fail()) { outfile.close(); return; }

    outfile.write(&static_data_copy, sizeof(AC_SPageStatic));
    if (outfile.fail()) { outfile.close(); return; }

    outfile.write(&save_spm, sizeof(double));

    uint64_t total_laps = data_to_save.size();
    outfile.write(&total_laps, sizeof(total_laps));

    uint64_t offsets_pos = outfile.tellp();
    std::vector<uint64_t> lap_offsets(total_laps, 0);
    outfile.write(lap_offsets.data(), total_laps * sizeof(uint64_t));

    for (uint64_t idx = 0; idx < data_to_save.size(); ++idx) {
        lap_offsets[idx] = outfile.tellp();
        data_to_save[idx].write_to_stream(outfile);
        data_to_save[idx] = AC_LapDataChannels();
    }
    std::vector<AC_LapDataChannels>().swap(data_to_save);

    outfile.seekp(offsets_pos);
    outfile.write(lap_offsets.data(), total_laps * sizeof(uint64_t));

    outfile.close_and_save(os_path, compression_method == COMPRESSION_ZSTD);
}

bool ACProvider::is_logging_active() const {
    return is_logging.load();
}

int ACProvider::get_provider_status() const {
    if (!dataGraphic) return 0;
    return dataGraphic->status;
}

void ACProvider::apply_math_conversions_in_place(AC_LapDataChannels& lap) {
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

Dictionary ACProvider::_lap_to_dict(const AC_LapDataChannels& c) {
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
    d["brakeBias"] = to_float_array(c.brakeBias);
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
    d["penaltyTime"] = to_float_array(c.penaltyTime);
    d["flag"] = to_int_array(c.flag);
    d["idealLineOn"] = to_int_array(c.idealLineOn);
    d["isInPitLane"] = to_int_array(c.isInPitLane);
    d["surfaceGrip"] = to_float_array(c.surfaceGrip);
    d["mandatoryPitDone"] = to_int_array(c.mandatoryPitDone);
    d["windSpeed"] = to_float_array(c.windSpeed);
    d["windDirection"] = to_float_array(c.windDirection);

    return d;
}

Dictionary ACProvider::get_live_snapshot() {
    if (!is_connected_flag || !dataPhysics || !dataGraphic || sessions_data.empty()) {
        return Dictionary();
    }
    std::lock_guard<std::mutex> lock(data_mutex);
    
    // copy last lap to prevent modification
    AC_LapDataChannels lap_copy = sessions_data.back();
    apply_math_conversions_in_place(lap_copy);
    return _lap_to_dict(lap_copy);
}

Dictionary ACProvider::get_lap_data(int lap_index) {
    if (lap_index < 0 || lap_index >= (int)loaded_visible_lap_offsets.size()) {
        return Dictionary();
    }

    if (cached_lap_index != lap_index) {
        TelemetryFile infile;
        if (!infile.open_read(loaded_session_file_path)) {
            return Dictionary();
        }

        cached_lap_index = -1;
        infile.seekg(loaded_visible_lap_offsets[lap_index]);
        cached_lap.read_from_stream(infile);
        bool failed = infile.fail();
        infile.close();

        if (failed || cached_lap.speedKmh.empty()) {
            cached_lap.clear();
            return Dictionary();
        }

        _unwrap_lap_positions(cached_lap);
        apply_math_conversions_in_place(cached_lap);
        cached_lap_index = lap_index;
    }

    return _lap_to_dict(cached_lap);
}

Dictionary ACProvider::get_session_metadata_from_file(const String& file_path) {
    AC_SPageStatic stat{};
    double spm = 0.0;
    uint64_t count = 0;
    int64_t ts = 0;
    std::vector<LapSummary> summaries;
    Dictionary sec_norm;
    Dictionary sec_m;
    double total_driven = 0.0;
    int session_type = -1;

    if (!_scan_session_summaries(file_path, stat, spm, count, ts, summaries, sec_norm, sec_m, total_driven, session_type)) {
        return Dictionary();
    }

    Dictionary ret = _build_session_metadata(stat, count, session_type, summaries, sec_norm, sec_m);
    ret["timestamp"] = ts;
    return ret;
}

Dictionary ACProvider::get_session_metadata() {
    if (!_ensure_summaries_loaded()) return Dictionary();
    return _build_session_metadata(loaded_session_static_data, loaded_session_lap_count, loaded_session_type, loaded_lap_summaries, loaded_sector_positions_norm, loaded_sector_positions_m);
}

void ACProvider::close_session() {
    loaded_session_file_path = "";
    loaded_session_lap_offsets.clear();
    loaded_session_lap_offsets.shrink_to_fit();
    loaded_visible_lap_offsets.clear();
    loaded_visible_lap_offsets.shrink_to_fit();
    loaded_session_samples_per_meter = 0.0;
    loaded_session_lap_count = -1;
    loaded_session_static_data = {};
    
    summaries_loaded = false;
    loaded_lap_summaries.clear();
    loaded_lap_summaries.shrink_to_fit();
    loaded_sector_positions_norm = Dictionary();
    loaded_sector_positions_m = Dictionary();
    loaded_total_driven_laps = 0.0;
    loaded_session_type = -1;

    cached_lap_index = -1;
    cached_lap = AC_LapDataChannels();
}

String ACProvider::get_internal_channel_name(const String& standard_name) {
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
    if (standard_name == "car_damage_front") return "carDamage_0";
    if (standard_name == "car_damage_rear") return "carDamage_1";
    if (standard_name == "car_damage_left") return "carDamage_2";
    if (standard_name == "car_damage_right") return "carDamage_3";
    if (standard_name == "car_damage_center") return "carDamage_4";
    if (standard_name == "tyres_out") return "numberOfTyresOut";

    return standard_name; // fallback
}

String ACProvider::_open_session_file(const String& file_path, TelemetryFile& infile, AC_SPageStatic& out_static, double& out_samples_per_meter, uint64_t& out_lap_count, std::vector<uint64_t>& out_lap_offsets, int64_t& out_timestamp) {
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
    infile.read(&out_static, sizeof(AC_SPageStatic));
    if (infile.fail()) {
        infile.close(); return "failed reading static data";
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

void ACProvider::_unwrap_lap_positions(AC_LapDataChannels& lap) const {
    // compatibility for old telemetry files (before commit 'e35eb69')
    if (!lap.normalizedCarPosition.empty()) {
        float base_offset = 0.0f;
        float prev_pos = lap.normalizedCarPosition[0];
        for (size_t j = 1; j < lap.normalizedCarPosition.size(); ++j) {
            float curr_pos = lap.normalizedCarPosition[j];
            if (curr_pos - prev_pos < -0.5f) {
                base_offset += 1.0f;
            } else if (curr_pos - prev_pos > 0.5f) {
                base_offset -= 1.0f;
            }
            prev_pos = curr_pos;
            lap.normalizedCarPosition[j] = curr_pos + base_offset;
        }
    }
}

bool ACProvider::_scan_session_summaries(const String& file_path, AC_SPageStatic& out_static, double& out_spm, uint64_t& out_lap_count, int64_t& out_timestamp, std::vector<LapSummary>& out_summaries, Dictionary& out_sec_norm, Dictionary& out_sec_m, double& out_total_driven, int& out_session_type) {
    out_summaries.clear();
    out_sec_norm = Dictionary();
    out_sec_m = Dictionary();
    out_total_driven = 0.0;
    out_session_type = -1;

    TelemetryFile infile;
    std::vector<uint64_t> offsets;
    String err = _open_session_file(file_path, infile, out_static, out_spm, out_lap_count, offsets, out_timestamp);
    if (!err.is_empty()) return false;

    out_summaries.reserve(out_lap_count);
    bool found_session_type = false;
    bool has_prev_pos = false;
    double prev_driven_pos = 0.0;

    AC_LapDataChannels lap;
    for (uint64_t i = 0; i < out_lap_count; ++i) {
        infile.seekg(offsets[i]);
        lap.read_from_stream(infile);
        if (infile.fail()) {
            infile.close();
            return false;
        }

        if (lap.speedKmh.empty()) continue;

        _unwrap_lap_positions(lap);

        if (!found_session_type && !lap.session.empty()) {
            out_session_type = lap.session[0];
            found_session_type = true;
        }

        for (size_t j = 0; j < lap.normalizedCarPosition.size(); j++) {
            double curr_pos = lap.normalizedCarPosition[j];
            if (has_prev_pos) {
                double diff = curr_pos - prev_driven_pos;
                if (diff < -0.5 && prev_driven_pos > 0.8 && curr_pos < 0.2) {
                    diff += 1.0;
                } else if (diff > 0.5 && prev_driven_pos < 0.2 && curr_pos > 0.8) {
                    diff -= 1.0;
                }
                if (std::abs(diff) > 0.1) {
                    diff = 0.0; // ignore large jumps
                }
                out_total_driven += diff;
            }
            prev_driven_pos = curr_pos;
            has_prev_pos = true;
        }

        LapSummary sum;
        sum.snapshot_count = (int)lap.speedKmh.size();
        sum.timestamp_count = (int)lap.timestamp.size();

        int current_sec_idx = lap.currentSectorIndex.empty() ? 0 : lap.currentSectorIndex[0];
        std::map<int, float> sec_vmax;
        std::map<int, float> sec_vmin;
        std::map<int, float> sec_speed_sum;
        std::map<int, int> sec_snapshot_count;
        std::map<int, int> sec_throttle_count;
        std::map<int, int> sec_brake_count;

        for (size_t j = 0; j < lap.speedKmh.size(); j++) {
            int loop_sec_idx = lap.currentSectorIndex.empty() ? 0 : lap.currentSectorIndex[j];

            if (!lap.currentSectorIndex.empty() && lap.currentSectorIndex[j] != current_sec_idx) {
                if (lap.lastSectorTime[j] > 0 && lap.lastSectorTime[j] <= lap.iCurrentTime[j] + 2000) {
                    sum.sector_times[current_sec_idx] = lap.lastSectorTime[j];
                }

                if (lap.currentSectorIndex[j] > current_sec_idx) {
                    if (!out_sec_norm.has(current_sec_idx) && j < lap.normalizedCarPosition.size()) {
                        float pos = lap.normalizedCarPosition[j];
                        out_sec_norm[current_sec_idx] = pos;
                        out_sec_m[current_sec_idx] = pos * out_static.trackSPlineLength;
                    }
                }

                current_sec_idx = lap.currentSectorIndex[j];
            }

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

            if (speed > sum.top_speed) {
                sum.top_speed = speed;
            }
        }
        sum.end_sec_idx = current_sec_idx;

        for (const auto& pair : sec_snapshot_count) {
            int s_idx = pair.first;
            int cnt = pair.second;
            if (cnt == 0) continue;

            Dictionary s_dict;
            s_dict["vmax_kmh"] = sec_vmax[s_idx];
            s_dict["vmin_kmh"] = sec_vmin[s_idx];
            s_dict["avg_speed_kmh"] = sec_speed_sum[s_idx] / cnt;
            s_dict["throttle_pct"] = (float)sec_throttle_count[s_idx] / cnt * 100.0f;
            s_dict["brake_pct"] = (float)sec_brake_count[s_idx] / cnt * 100.0f;

            sum.sectors_data[s_idx] = s_dict;
        }

        if (!lap.normalizedCarPosition.empty()) {
            sum.has_positions = true;
            sum.pos_count = lap.normalizedCarPosition.size();
            sum.first_pos = lap.normalizedCarPosition.front();
            sum.last_pos = lap.normalizedCarPosition.back();

            float total_progression = 0.0f;
            for (size_t k = 1; k < lap.normalizedCarPosition.size(); k++) {
                float diff = lap.normalizedCarPosition[k] - lap.normalizedCarPosition[k - 1];
                if (diff < -0.5f) diff += 1.0f;
                if (diff > 0.5f) diff -= 1.0f;
                if (diff > 0.0f && diff < 0.1f) {
                    total_progression += diff;
                }
            }
            sum.total_progression = total_progression;
        }

        size_t min_size = std::min({lap.numberOfTyresOut.size(), lap.penaltyTime.size(), lap.flag.size(), lap.isInPitLane.size()});
        for (size_t k = 0; k < min_size; k++) {
            if (lap.numberOfTyresOut[k] >= 4 || lap.penaltyTime[k] > 0.0f || lap.flag[k] == AC_PENALTY_FLAG || lap.isInPitLane[k] == 1) {
                sum.has_penalty_or_offtrack = true;
                break;
            }
        }

        if (!lap.iCurrentTime.empty()) {
            sum.has_iCurrentTime = true;
            sum.back_iCurrentTime = lap.iCurrentTime.back();
            int max_ct = 0;
            for (int t : lap.iCurrentTime) {
                if (t > max_ct && t < INT_MAX) max_ct = t;
            }
            sum.max_iCurrentTime = max_ct;
        }

        if (!lap.iLastTime.empty()) {
            sum.has_iLastTime = true;
            std::map<int, int> counts;
            int max_cnt = 0;
            int most_freq = 0;
            int max_lt = 0;
            for (int t : lap.iLastTime) {
                if (t > max_lt) max_lt = t;
                if (t > 0 && t < INT_MAX) {
                    counts[t]++;
                    if (counts[t] > max_cnt) {
                        max_cnt = counts[t];
                        most_freq = t;
                    }
                }
            }
            sum.most_freq_iLastTime = most_freq;
            sum.max_iLastTime = max_lt;
        }

        if (!lap.iBestTime.empty()) {
            sum.has_iBestTime = true;
            sum.first_iBestTime = lap.iBestTime[0];
        }

        if (!lap.currentSectorIndex.empty() && !lap.lastSectorTime.empty()) {
            size_t n = std::min(lap.currentSectorIndex.size(), lap.lastSectorTime.size());
            for (size_t k = 0; k < n; k++) {
                if (lap.currentSectorIndex[k] == 0 && lap.lastSectorTime[k] > 0) {
                    sum.first_sec0_last_sector_time = lap.lastSectorTime[k];
                    break;
                }
            }
        }

        if (!lap.fuel.empty()) {
            double consumed = 0.0;
            float prev_fuel = lap.fuel[0];
            for (size_t k = 1; k < lap.fuel.size(); k++) {
                float current_fuel = lap.fuel[k];
                if (current_fuel < prev_fuel) {
                    consumed += (prev_fuel - current_fuel);
                }
                prev_fuel = current_fuel;
            }
            sum.fuel_consumed = consumed;
        }

        out_summaries.push_back(std::move(sum));
    }

    infile.close();
    return true;
}

bool ACProvider::_ensure_summaries_loaded() {
    if (summaries_loaded) return true;
    if (loaded_session_file_path.is_empty()) return false;

    AC_SPageStatic stat{};
    double spm = 0.0;
    uint64_t count = 0;
    int64_t ts = 0;
    if (!_scan_session_summaries(loaded_session_file_path, stat, spm, count, ts, loaded_lap_summaries, loaded_sector_positions_norm, loaded_sector_positions_m, loaded_total_driven_laps, loaded_session_type)) {
        return false;
    }

    summaries_loaded = true;
    return true;
}

Dictionary ACProvider::_build_session_metadata(const AC_SPageStatic& stat, uint64_t count, int session_type, const std::vector<LapSummary>& summaries, const Dictionary& sector_positions_norm, const Dictionary& sector_positions_m) const {
    Dictionary meta;

    meta["sim_id"] = "AC";
    meta["track_name"] = wchar_to_gdstring(stat.track, 33).to_lower();
    meta["track_config"] = wchar_to_gdstring(stat.trackConfiguration, 33);
    meta["car_name"] = wchar_to_gdstring(stat.carModel, 33);
    meta["total_laps"] = (int)count;
    meta["sector_count"] = stat.sectorCount;
    meta["session_type"] = session_type;

    Array laps_arr;
    int best_lap_time = 0;

    for (size_t i = 0; i < summaries.size(); i++) {
        const LapSummary& sum = summaries[i];
        Dictionary lap_stats;
        int lap_time = 0;
        Dictionary sector_times = sum.sector_times.duplicate();

        bool is_completed = false;
        if (sum.pos_count > 1 && sum.total_progression > 0.85f) {
            if (i + 1 < summaries.size() && summaries[i + 1].has_positions) {
                // check if it wrapped around the finish line
                if (sum.last_pos > 0.90f && summaries[i + 1].first_pos < 0.10f) {
                    is_completed = true;
                }
            }
        }

        bool is_valid = is_completed && !sum.has_penalty_or_offtrack;
        int next_lap_best_time = 0;
        int real_lap_time = 0;

        // try getting exact time from next lap if exists
        if (i + 1 < summaries.size()) {
            const LapSummary& next_sum = summaries[i + 1];

            if (next_sum.has_iBestTime) {
                next_lap_best_time = next_sum.first_iBestTime;
            }

            if (next_sum.has_iLastTime) {
                int next_most_frequent = next_sum.most_freq_iLastTime;
                int curr_most_frequent = sum.most_freq_iLastTime;

                if (next_most_frequent > 0) {
                    if (curr_most_frequent > 0 && next_most_frequent == curr_most_frequent) {
                        if (std::abs(next_most_frequent - sum.max_iCurrentTime) > 50) {
                            is_valid = false;
                        } else {
                            real_lap_time = next_most_frequent;
                        }
                    } else {
                        real_lap_time = next_most_frequent;
                    }
                }
            }

            if (is_completed && next_sum.first_sec0_last_sector_time > 0) {
                sector_times[sum.end_sec_idx] = next_sum.first_sec0_last_sector_time;
            }
        }

        if (real_lap_time > 0) {
            lap_time = real_lap_time;
        } else if (sum.has_iCurrentTime) {
            lap_time = sum.max_iCurrentTime;
        } else {
            lap_time = 0;
            is_completed = false;
        }

        if (is_valid) {
            // check if it's best. if it is, next lap's best time will be this lap's time
            if (sum.has_iBestTime && sum.first_iBestTime > 0 && lap_time > 0 && lap_time < sum.first_iBestTime) {
                if (next_lap_best_time > 0 && next_lap_best_time != lap_time) {
                    is_valid = false;
                }
            }
        }

        lap_stats["lap_time_ms"] = lap_time;
        lap_stats["sector_times_ms"] = sector_times;
        lap_stats["top_speed_kmh"] = sum.top_speed;
        lap_stats["snapshot_count"] = sum.snapshot_count;
        lap_stats["is_completed"] = is_completed;
        lap_stats["is_valid"] = is_valid;
        lap_stats["sectors_data"] = sum.sectors_data.duplicate(true);

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
    meta["sector_positions_norm"] = sector_positions_norm.duplicate();
    meta["sector_positions_m"] = sector_positions_m.duplicate();
    meta["laps"] = laps_arr;

    return meta;
}

Dictionary ACProvider::_static_to_dict(const AC_SPageStatic &s) {
    Dictionary dict;
    
    // raw ac keys
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
    dict["carSkin"] = wchar_to_gdstring(s.carSkin, 33);
    dict["reversedGridPositions"] = s.reversedGridPositions;
    dict["pitWindowStart"] = s.pitWindowStart;
    dict["pitWindowEnd"] = s.pitWindowEnd;
    dict["isOnline"] = s.isOnline;

    // standard multi-sim keys
    // TODO: it can be expanded later
    dict["car_model"] = wchar_to_gdstring(s.carModel, 33);
    dict["track_name"] = wchar_to_gdstring(s.track, 33).to_lower();
    dict["track_config"] = wchar_to_gdstring(s.trackConfiguration, 33);
    dict["track_length"] = s.trackSPlineLength;
    dict["max_rpm"] = s.maxRpm;
    dict["sector_count"] = s.sectorCount;

    return dict;
}

void ACProvider::logging_loop() {
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(0.001));
    auto next_tick = std::chrono::steady_clock::now();

    timeBeginPeriod(1);

    int stale_counter = 0;
    int graphic_stale_counter = 0;
    int local_last_graphic_packet_id = -1;
    int max_stale_ticks = 3000; // 3 secs timeout
    int current_session_type = -1;

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
            } else if (dataGraphic->session != AC_UNKNOWN && dataGraphic->session != current_session_type) {
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
                    
                    std::vector<AC_LapDataChannels> data_to_save = std::move(sessions_data);
                    sessions_data.clear();
                    AC_SPageStatic static_copy = *dataStatic;
                    double save_spm = samples_per_meter;
                    
                    std::thread([this, data_to_save = std::move(data_to_save), static_copy, save_spm, final_path]() mutable {
                        this->_flush_sessions_to_disk(std::move(data_to_save), static_copy, save_spm, final_path);
                    }).detach();
                }
                
                current_session_type = dataGraphic->session;
                last_lap_count = 0;
                last_recorded_meter = -1.0;
            }

            if (dataGraphic->status == AC_OFF) {
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
            
            
            if (dataGraphic->status != AC_LIVE) {
                stale_counter = 0;
                std::this_thread::sleep_until(next_tick);
                continue;
            }

            if (dataPhysics->packetId == last_physics_packet_id) {
                std::this_thread::sleep_until(next_tick);
                continue;
            }
            last_physics_packet_id = dataPhysics->packetId;

            {
                std::lock_guard<std::mutex> lock(data_mutex);

                bool lap_changed = false;
                if (dataGraphic->iCurrentTime < last_i_current_time) {
                    lap_changed = true;
                }
                last_i_current_time = dataGraphic->iCurrentTime;
                
                if (dataGraphic->completedLaps > last_lap_count) {
                    lap_changed = true;
                    last_lap_count = dataGraphic->completedLaps;
                }

                // detect spatial lap crossing to avoid delayed lap timers
                if (!sessions_data.empty() && !sessions_data.back().normalizedCarPosition.empty()) {
                    float last_pos = sessions_data.back().normalizedCarPosition.back();
                    if (last_pos > 0.8f && dataGraphic->normalizedCarPosition < 0.2f) {
                        lap_changed = true;
                    }
                    
                    float diff = dataGraphic->normalizedCarPosition - last_pos;
                    if (diff < -0.5f) diff += 1.0f;
                    if (diff > 0.5f) diff -= 1.0f;
                    
                    float teleport_threshold = 0.1f;
                    if (dataStatic->trackSPlineLength > 0.0) {
                        teleport_threshold = 200.0f / dataStatic->trackSPlineLength; // 200 meters
                    }

                    if (std::abs(diff) > teleport_threshold) {
                        lap_changed = true;
                    }
                }

                if (lap_changed || sessions_data.empty()) {
                    bool should_push = true;
                    if (!sessions_data.empty()) {
                        auto& last_lap = sessions_data.back();
                        if (last_lap.timestamp.size() < 10) {
                            should_push = false;
                        }
                    }

                    if (should_push) {
                        sessions_data.push_back(AC_LapDataChannels());
                    } else {
                        sessions_data.back().clear();
                    }
                    last_recorded_meter = -INFINITY;
                }

                // track continuous spline distance
                // graphic position updates at 60Hz, physics at 333Hz.
                // using graphic pos directly causes staircase artifacts.
                // we integrate physics speed for smooth high-frequency distance
                // and soft-correct it to graphic pos.
                static double internal_meter = -1.0;
                
                if (last_recorded_meter == -INFINITY) {
                    internal_meter = -1.0; // reset on new lap
                }
                
                double graphic_spline_pos = dataGraphic->normalizedCarPosition * dataStatic->trackSPlineLength;
                
                if (internal_meter < 0.0 || dataStatic->trackSPlineLength <= 0.0) {
                    internal_meter = graphic_spline_pos;
                } else {
                    int packet_diff = dataPhysics->packetId - last_physics_packet_id;
                    if (packet_diff > 0 && packet_diff < 100) {
                        double dt = packet_diff / 333.333333333; // 333Hz physics tick
                        internal_meter += (dataPhysics->speedKmh / 3.6) * dt;
                    }
                    
                    // complementary filter: soft-correct towards graphic_spline_pos
                    double diff = graphic_spline_pos - internal_meter;
                    // handle track wrap-around
                    if (diff < -dataStatic->trackSPlineLength / 2.0) diff += dataStatic->trackSPlineLength;
                    if (diff > dataStatic->trackSPlineLength / 2.0) diff -= dataStatic->trackSPlineLength;
                    
                    if (std::abs(diff) < 20.0) {
                        internal_meter += diff * 0.05; // 5% correction per tick
                    } else {
                        internal_meter = graphic_spline_pos; // hard reset on big jump
                    }
                }
                
                if (dataStatic->trackSPlineLength > 0.0) {
                    while (internal_meter >= dataStatic->trackSPlineLength) internal_meter -= dataStatic->trackSPlineLength;
                    while (internal_meter < 0.0) internal_meter += dataStatic->trackSPlineLength;
                }

                double spline_pos = internal_meter;
                double dist_diff = 0.0;
                
                if (last_recorded_meter >= 0.0) {
                    dist_diff = spline_pos - last_recorded_meter;
                    // fix distance jump when track spline loops
                    if (dist_diff < -dataStatic->trackSPlineLength / 2.0) {
                        dist_diff += dataStatic->trackSPlineLength;
                    } else if (dist_diff > dataStatic->trackSPlineLength / 2.0) {
                        dist_diff -= dataStatic->trackSPlineLength;
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
                        if (jump_size > 20.0 && dataStatic->trackSPlineLength > 0.0 && 
                            jump_size < dataStatic->trackSPlineLength - 20.0) {
                            last_recorded_meter = current_meter;
                        } else {
                            direction = (current_meter > last_recorded_meter) ? 1.0 : -1.0;
                            last_recorded_meter += distance_threshold * direction;
                            
                            if (dataStatic->trackSPlineLength > 0.0) {
                                if (last_recorded_meter >= dataStatic->trackSPlineLength) {
                                    last_recorded_meter -= dataStatic->trackSPlineLength;
                                } else if (last_recorded_meter < 0.0) {
                                    last_recorded_meter += dataStatic->trackSPlineLength;
                                }
                            }
                        }
                    }
                    
                    auto& lap = sessions_data.back();

                    // time interpolation pass
                    // since we trigger records based on high-frequency distance (complementary filter above),
                    // we must also interpolate the 60hz graphic time to match the exact moment of this sample.
                    // we use the 333hz physics packet difference to smoothly advance the clock.
                    double smoothed_timestamp = dataGraphic->iCurrentTime / 1000.0;
                    int32_t smoothed_iCurrentTime = dataGraphic->iCurrentTime;
                    double smoothed_normalizedCarPosition = dataGraphic->normalizedCarPosition;
                    float smoothed_distanceTraveled = dataGraphic->distanceTraveled;

                    if (dataStatic->trackSPlineLength > 0.0) {
                        smoothed_normalizedCarPosition = last_recorded_meter / dataStatic->trackSPlineLength;
                    }

                    if (!lap.timestamp.empty() && old_last_recorded_meter >= 0.0) {
                        int packet_diff = dataPhysics->packetId - lap.packetId_physics.back();
                        if (packet_diff > 0 && packet_diff < 1000) {
                            double dt = packet_diff * (1.0 / 333.333333333); // ac physics is 333hz
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

                    // graphic
                    lap.status.push_back(dataGraphic->status);
                    lap.session.push_back(dataGraphic->session);
                    lap.position.push_back(dataGraphic->position);
                    lap.sessionTimeLeft.push_back(dataGraphic->sessionTimeLeft);
                    lap.isInPit.push_back(dataGraphic->isInPit);
                    lap.currentSectorIndex.push_back(dataGraphic->currentSectorIndex);
                    lap.lastSectorTime.push_back(dataGraphic->lastSectorTime);
                    lap.carCoordinates_x.push_back(dataGraphic->carCoordinates[0]);
                    lap.carCoordinates_y.push_back(dataGraphic->carCoordinates[1]);
                    lap.carCoordinates_z.push_back(dataGraphic->carCoordinates[2]);
                    lap.penaltyTime.push_back(dataGraphic->penaltyTime);
                    lap.flag.push_back(dataGraphic->flag);
                    lap.idealLineOn.push_back(dataGraphic->idealLineOn);
                    lap.isInPitLane.push_back(dataGraphic->isInPitLane);
                    lap.surfaceGrip.push_back(dataGraphic->surfaceGrip);
                    lap.mandatoryPitDone.push_back(dataGraphic->mandatoryPitDone);
                    lap.windSpeed.push_back(dataGraphic->windSpeed);
                    lap.windDirection.push_back(dataGraphic->windDirection);
                }
            }
        }

        std::this_thread::sleep_until(next_tick);
    }

    timeEndPeriod(1);
}

#pragma comment(lib, "winmm.lib")

String ACProvider::load_session(const String& file_path) {
    close_session();

    TelemetryFile infile;
    uint64_t count = 0;
    int64_t dummy_ts = 0;
    String err = _open_session_file(file_path, infile, loaded_session_static_data, loaded_session_samples_per_meter, count, loaded_session_lap_offsets, dummy_ts);
    if (!err.is_empty()) {
        close_session();
        return err;
    }

    uint64_t logical_size = infile.get_size();
    infile.close();

    loaded_session_file_path = file_path;
    loaded_session_lap_count = (int)count;

    loaded_visible_lap_offsets.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t start = loaded_session_lap_offsets[i];
        uint64_t end = (i + 1 < count) ? loaded_session_lap_offsets[i + 1] : logical_size;
        if (end >= start && end - start > sizeof(uint64_t)) {
            loaded_visible_lap_offsets.push_back(start);
        }
    }

    return "";
}

Dictionary ACProvider::get_live_static_data() {
    if (!is_connected_flag || !dataStatic) return Dictionary();
    return _static_to_dict(*dataStatic);
}

Dictionary ACProvider::get_loaded_session_static_data() {
    return _static_to_dict(loaded_session_static_data);
}

double ACProvider::get_loaded_session_lap_fuel_consumption(int lap_index) {
    if (!_ensure_summaries_loaded()) return 0.0;
    if (lap_index < 0 || lap_index >= (int)loaded_lap_summaries.size()) return 0.0;
    return loaded_lap_summaries[lap_index].fuel_consumed;
}

double ACProvider::get_loaded_session_total_fuel_consumption() {
    if (!_ensure_summaries_loaded()) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < loaded_lap_summaries.size(); i++) {
        total += loaded_lap_summaries[i].fuel_consumed;
    }
    return total;
}

double ACProvider::get_loaded_session_total_laps() {
    if (!_ensure_summaries_loaded()) return 0.0;
    return loaded_total_driven_laps;
}

Dictionary ACProvider::get_loaded_session_lap_stats(int lap_index) {
    Dictionary stats;
    if (!_ensure_summaries_loaded()) return stats;
    if (lap_index < 0 || lap_index >= (int)loaded_lap_summaries.size()) return stats;

    const auto &sum = loaded_lap_summaries[lap_index];
    if (sum.timestamp_count == 0) return stats;

    int lap_time = 0;
    Dictionary sector_times = sum.sector_times.duplicate();
    bool is_completed = sum.pos_count > 1 && sum.total_progression > 0.90f;

    // try to get the exact lap_time and final sector time from the next lap
    if (lap_index + 1 < (int)loaded_lap_summaries.size() && loaded_lap_summaries[lap_index + 1].timestamp_count > 0) {
        const auto &next_sum = loaded_lap_summaries[lap_index + 1];

        lap_time = next_sum.max_iLastTime;
        if (lap_time == 0 && sum.has_iCurrentTime) {
            lap_time = sum.back_iCurrentTime;
        }

        if (is_completed && next_sum.first_sec0_last_sector_time > 0) {
            sector_times[sum.end_sec_idx] = next_sum.first_sec0_last_sector_time;
        }
    } else {
        lap_time = sum.has_iCurrentTime ? sum.back_iCurrentTime : 0;
        is_completed = false;
    }

    stats["lap_time_ms"] = lap_time;
    stats["sector_times_ms"] = sector_times;
    stats["top_speed_kmh"] = sum.top_speed;
    stats["snapshot_count"] = sum.timestamp_count;
    stats["is_completed"] = is_completed;

    return stats;
}

Dictionary ACProvider::calculate_lap_time_delta(const String& target_file_path, int target_lap_index, const String& current_file_path, int current_lap_index, const PackedFloat32Array& reference_positions) {
    PackedFloat32Array cumulative_delta;
    Dictionary result;
    result["cumulative_delta"] = PackedFloat32Array();
    result["delta_rate"] = PackedFloat32Array();

    if (target_file_path.is_empty() || target_lap_index < 0 || current_lap_index < 0) return result;
    
    String actual_curr_path = current_file_path.is_empty() ? target_file_path : current_file_path;

    double session_spm = 1.0;

    auto load_lap = [this, &session_spm](String file_path, int lap_index, AC_LapDataChannels &out_lap) -> bool {
        TelemetryFile infile;
        AC_SPageStatic stat;
        double spm;
        uint64_t count;
        std::vector<uint64_t> offsets;

        int64_t timestamp = 0;
        if (!_open_session_file(file_path, infile, stat, spm, count, offsets, timestamp).is_empty()) {
            return false;
        }

        if (lap_index < 0 || lap_index >= count) return false;

        session_spm = spm;

        infile.seekg(offsets[lap_index]);
        out_lap.read_from_stream(infile);
        return true;
    };

    AC_LapDataChannels target_lap, current_lap;
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
