#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>
#include "../../src/acc_data_structs.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: acc_sector_dump <telemetry.acct>\n";
        return 1;
    }
    
    std::string filename = argv[1];
    
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "error opening file.\n";
        return 1;
    }
    
    // read sig
    char sig[4];
    in.read(sig, 4);
    if (std::string(sig, 4) != "ACCT") {
        std::cerr << "invalid signature: " << std::string(sig, 4) << "\n";
        return 1;
    }

    // read static
    ACC_SPageStatic static_data;
    in.read(reinterpret_cast<char*>(&static_data), sizeof(ACC_SPageStatic));
    
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
    
    std::cout << "=== ACC SECTOR DIAGNOSTIC DUMP ===\n";
    std::cout << "total laps in file: " << total_laps << "\n";
    std::cout << "sector count: " << static_data.sectorCount << "\n\n";
    
    for (uint64_t l = 0; l < total_laps; ++l) {
        in.seekg(lap_offsets[l]);
        ACC_LapDataChannels data;
        data.read_from_stream(in);
        
        size_t size = data.speedKmh.size();
        std::cout << "=== LAP " << l << " (samples: " << size << ") ===\n";
        
        if (size == 0) {
            std::cout << "  [empty]\n\n";
            continue;
        }
        
        // print iCurrentTime range
        if (!data.iCurrentTime.empty()) {
            int min_t = data.iCurrentTime[0], max_t = data.iCurrentTime[0];
            for (int t : data.iCurrentTime) {
                if (t < min_t) min_t = t;
                if (t > max_t) max_t = t;
            }
            std::cout << "  iCurrentTime: first=" << data.iCurrentTime[0] 
                      << " last=" << data.iCurrentTime.back()
                      << " min=" << min_t << " max=" << max_t << "\n";
        }
        
        // print iLastTime range
        if (!data.iLastTime.empty()) {
            int min_t = data.iLastTime[0], max_t = data.iLastTime[0];
            for (int t : data.iLastTime) {
                if (t < min_t) min_t = t;
                if (t > max_t) max_t = t;
            }
            std::cout << "  iLastTime: first=" << data.iLastTime[0] 
                      << " last=" << data.iLastTime.back()
                      << " min=" << min_t << " max=" << max_t << "\n";
        }
        
        // print normalizedCarPosition range
        if (!data.normalizedCarPosition.empty()) {
            std::cout << "  normPos: first=" << std::setprecision(5) << data.normalizedCarPosition[0]
                      << " last=" << data.normalizedCarPosition.back() << "\n";
        }
        
        // print all sector index transitions and lastSectorTime changes
        std::cout << "  --- sector transitions & lastSectorTime changes ---\n";
        
        int prev_sec = -1;
        int prev_lst = -1;
        std::string prev_split = "";
        
        for (size_t i = 0; i < size; i++) {
            int sec = data.currentSectorIndex.empty() ? 0 : data.currentSectorIndex[i];
            int lst = (data.lastSectorTime.size() > i) ? data.lastSectorTime[i] : 0;
            
            bool sec_changed = (prev_sec >= 0 && sec != prev_sec);
            bool lst_changed = (lst != prev_lst && prev_lst >= 0);
            
            if (sec_changed || lst_changed) {
                float pos = (data.normalizedCarPosition.size() > i) ? data.normalizedCarPosition[i] : -1.0f;
                int ict = (data.iCurrentTime.size() > i) ? data.iCurrentTime[i] : -1;
                
                std::cout << "  [" << std::setw(6) << i << "] ";
                if (sec_changed) {
                    std::cout << "SECTOR " << prev_sec << "->" << sec << "  ";
                }
                if (lst_changed) {
                    std::cout << "lastSecTime " << prev_lst << "->" << lst << "  ";
                }
                std::cout << "pos=" << std::fixed << std::setprecision(5) << pos 
                          << " iCurTime=" << ict << "\n";
            }
            
            prev_sec = sec;
            prev_lst = lst;
        }
        
        std::cout << "\n";
    }
    
    return 0;
}
