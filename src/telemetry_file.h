#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <fstream>
#include <godot_cpp/variant/string.hpp>

class TelemetryFile {
private:
    std::vector<uint8_t> buffer;
    uint64_t rw_pos = 0;
    bool is_writing = false;
    bool is_open_flag = false;
    bool file_failed = false;

public:
    TelemetryFile();
    ~TelemetryFile();

    bool open_write();
    bool open_read(const godot::String& path);
    
    bool close_and_save(const godot::String& path, bool compress = true);
    static bool compress_existing_file(const godot::String& path);
    void close();

    bool is_open() const { return is_open_flag; }
    bool fail() const { return file_failed; }
    uint64_t get_size() const { return buffer.size(); }

    template<typename T>
    TelemetryFile& write(const T* data, size_t size) {
        if (!is_writing || !is_open_flag || file_failed) {
            file_failed = true;
            return *this;
        }
        
        if (rw_pos + size > buffer.size()) {
            buffer.resize(rw_pos + size);
        }
        
        memcpy(buffer.data() + rw_pos, data, size);
        rw_pos += size;
        return *this;
    }

    template<typename T>
    TelemetryFile& read(T* data, size_t size) {
        if (is_writing || !is_open_flag || file_failed) {
            file_failed = true;
            return *this;
        }

        if (rw_pos + size > buffer.size()) {
            file_failed = true;
            return *this;
        }

        memcpy(data, buffer.data() + rw_pos, size);
        rw_pos += size;
        return *this;
    }

    // read
    TelemetryFile& seekg(uint64_t pos);
    
    // write
    TelemetryFile& seekp(uint64_t pos);

    // get read pos
    uint64_t tellg() const;
    
    // get write pos
    uint64_t tellp() const;
};
