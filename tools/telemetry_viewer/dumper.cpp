#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>
#include "../../src/ac_data_structs.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: dumper <telemetry.bin> <track_length>\n";
        return 1;
    }
    
    std::string filename = argv[1];
    float track_length = std::stof(argv[2]);
    
    AC_LapDataChannels data;
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Error opening file.\n";
        return 1;
    }
    
    // read signature
    std::string signature = "ACTL";
    std::vector<char> sig_buffer(signature.length());
    in.read(sig_buffer.data(), signature.length());
    std::string read_sig(sig_buffer.data(), signature.length());
    if (read_sig != signature) {
        std::cerr << "Invalid signature.\n";
        return 1;
    }

    // read static
    AC_SPageStatic static_data;
    in.read(reinterpret_cast<char*>(&static_data), sizeof(AC_SPageStatic));
    
    // read samples per meter
    double samples_per_meter;
    in.read(reinterpret_cast<char*>(&samples_per_meter), sizeof(double));
    
    // read total laps
    uint64_t total_laps = 0;
    in.read(reinterpret_cast<char*>(&total_laps), sizeof(uint64_t));
    
    // read lap offsets
    std::vector<uint64_t> lap_offsets(total_laps);
    if (total_laps > 0) {
        in.read(reinterpret_cast<char*>(lap_offsets.data()), total_laps * sizeof(uint64_t));
    }
    
    // csv header
    std::cout << "Lap,Index,Meters,NormPos,SpeedKmh,Gear,RPM,Gas,Brake,Steer,LapTime_ms,Sector\n";
    
    for (uint64_t l = 0; l < total_laps; ++l) {
        in.seekg(lap_offsets[l]);
        data.read_from_stream(in);
        
        size_t size = data.normalizedCarPosition.size();
        for (size_t i = 0; i < size; ++i) {
            float norm_pos = data.normalizedCarPosition[i];
            float meters = norm_pos * track_length;
            
            std::cout << l << ","
                      << i << ","
                      << std::fixed << std::setprecision(2) << meters << ","
                      << std::setprecision(5) << norm_pos << ","
                      << std::setprecision(1) << data.speedKmh[i] << ","
                      << data.gear[i] << ","
                      << data.rpms[i] << ","
                      << std::setprecision(3) << data.gas[i] << ","
                      << std::setprecision(3) << data.brake[i] << ","
                      << std::setprecision(3) << data.steerAngle[i] << ","
                      << data.iCurrentTime[i] << ","
                      << data.currentSectorIndex[i] << "\n";
        }
    }
    return 0;
}
