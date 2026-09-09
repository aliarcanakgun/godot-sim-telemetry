#include "telemetry_file.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <cstring>
#include "zstd.h"

TelemetryFile::TelemetryFile() {}

TelemetryFile::~TelemetryFile() {
    close();
}

bool TelemetryFile::open_write() {
    buffer.clear();
    rw_pos = 0;
    is_writing = true;
    is_open_flag = true;
    file_failed = false;
    return true;
}

bool TelemetryFile::open_read(const godot::String& path) {
    is_writing = false;
    rw_pos = 0;
    buffer.clear();
    file_failed = false;
    
    godot::String os_path = path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = godot::ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    std::ifstream infile(os_path.utf8().get_data(), std::ios::binary);
    if (!infile.is_open()) {
        file_failed = true;
        is_open_flag = false;
        return false;
    }
    
    infile.seekg(0, std::ios::end);
    size_t file_size = infile.tellg();
    infile.seekg(0, std::ios::beg);
    
    if (file_size < 4) {
        file_failed = true;
        is_open_flag = false;
        return false;
    }
    
    char first_sig[4];
    infile.read(first_sig, 4);
    
    if (file_size >= 8) {
        char magic[4];
        infile.read(magic, 4);
        
        if (std::strncmp(magic, "ZST2", 4) == 0) { // compressed file
            uint64_t uncompressed_size = 0;
            infile.read(reinterpret_cast<char*>(&uncompressed_size), 8);
            
            size_t comp_size = file_size - 16;
            std::vector<uint8_t> comp_data(comp_size);
            infile.read(reinterpret_cast<char*>(comp_data.data()), comp_size);
            
            std::vector<uint8_t> decomp_data(uncompressed_size);
            size_t dSize = ZSTD_decompress(decomp_data.data(), uncompressed_size, comp_data.data(), comp_size);
            if (ZSTD_isError(dSize) || dSize != uncompressed_size) {
                file_failed = true;
                is_open_flag = false;
                return false; // decompression failed
            }
            
            buffer.resize(4 + uncompressed_size);
            std::memcpy(buffer.data(), first_sig, 4);
            std::memcpy(buffer.data() + 4, decomp_data.data(), uncompressed_size);
            
            is_open_flag = true;
            return true;
        }
    }
    
    // Uncompressed file fallback
    buffer.resize(file_size);
    infile.seekg(0, std::ios::beg);
    infile.read(reinterpret_cast<char*>(buffer.data()), file_size);
    
    is_open_flag = true;
    return true;
}

bool TelemetryFile::close_and_save(const godot::String& path, bool compress) {
    if (!is_writing || !is_open_flag || file_failed) return false;
    
    godot::String os_path = path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = godot::ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    
    std::string final_path_str = os_path.utf8().get_data();
    std::string tmp_path_str = final_path_str + ".tmp";
    
    std::ofstream outfile(tmp_path_str, std::ios::binary);
    if (!outfile.is_open()) {
        file_failed = true;
        return false;
    }
    
    if (compress && buffer.size() > 4) {
        outfile.write(reinterpret_cast<const char*>(buffer.data()), 4); // original signature
        outfile.write("ZST2", 4);
        
        uint64_t uncompressed_payload_size = buffer.size() - 4;
        outfile.write(reinterpret_cast<const char*>(&uncompressed_payload_size), 8);
        
        size_t bound = ZSTD_compressBound(uncompressed_payload_size);
        std::vector<uint8_t> comp_data(bound);
        size_t cSize = ZSTD_compress(comp_data.data(), bound, buffer.data() + 4, uncompressed_payload_size, 1);
        
        if (ZSTD_isError(cSize)) {
            // fallback to uncompressed if zstd fails
            outfile.seekp(0, std::ios::beg);
            outfile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        } else {
            outfile.write(reinterpret_cast<const char*>(comp_data.data()), cSize);
        }
    } else {
        outfile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    }
    
    outfile.close();
    
    if (outfile.fail()) {
        std::remove(tmp_path_str.c_str());
        return false;
    }
    
    std::remove(final_path_str.c_str());
    if (std::rename(tmp_path_str.c_str(), final_path_str.c_str()) != 0) {
        return false;
    }
    
    close();
    return true;
}

bool TelemetryFile::compress_existing_file(const godot::String& path) {
    TelemetryFile file;
    if (!file.open_read(path)) return false;
    
    file.is_writing = true; // allow close_and_save to work
    file.is_open_flag = true;
    return file.close_and_save(path, true);
}

bool TelemetryFile::uncompress_existing_file(const godot::String& path) {
    TelemetryFile file;
    if (!file.open_read(path)) return false;
    
    file.is_writing = true; // allow close_and_save to work
    file.is_open_flag = true;
    return file.close_and_save(path, false);
}

void TelemetryFile::close() {
    is_open_flag = false;
    buffer.clear();
    rw_pos = 0;
}

TelemetryFile& TelemetryFile::seekg(uint64_t pos) {
    if (!is_open_flag || file_failed) return *this;
    rw_pos = pos;
    return *this;
}

TelemetryFile& TelemetryFile::seekp(uint64_t pos) {
    if (!is_open_flag || file_failed) return *this;
    rw_pos = pos;
    return *this;
}

uint64_t TelemetryFile::tellg() const {
    return rw_pos;
}

uint64_t TelemetryFile::tellp() const {
    return rw_pos;
}
