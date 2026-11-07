#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <godot_cpp/variant/string.hpp>

struct ZSTD_DCtx_s;

class TelemetryFile {
private:
    // write mode buffer
    std::vector<uint8_t> buffer;
    uint64_t rw_pos = 0;
    bool is_writing = false;
    bool is_open_flag = false;
    bool file_failed = false;

    // streaming read mode
    // the logical byte space is identical to the in-memory buffer used when
    // writing: 4 raw sig bytes + uncompressed payload. lap offsets stored
    // in session files refer to this space, so they keep working unchanged.
    bool is_streaming = false;
    bool stream_compressed = false;
    std::ifstream in_file;
    uint64_t stream_size = 0;       // logical size: header + payload
    uint64_t stream_pos = 0;        // bytes produced so far
    uint8_t head[4] = {0, 0, 0, 0}; // first four uncompressed bytes
    ZSTD_DCtx_s* dctx = nullptr;
    std::vector<uint8_t> in_buf;
    size_t in_buf_pos = 0;
    size_t in_buf_size = 0;
    bool in_eof = false;
    std::vector<uint8_t> scratch;

    static constexpr uint64_t COMPRESSED_HEADER_SIZE = 16; // sig(4) + "ZST2"(4) + u64 size(8)

    void close_stream();
    bool stream_read(void* dst, uint64_t size);
    bool stream_rewind();
    bool stream_refill();
    bool stream_decompress(uint8_t* dst, uint64_t size);
    bool stream_skip(uint64_t size);

public:
    TelemetryFile();
    ~TelemetryFile();

    TelemetryFile(const TelemetryFile&) = delete;
    TelemetryFile& operator=(const TelemetryFile&) = delete;

    bool open_write();
    bool open_read(const godot::String& path);
    
    bool close_and_save(const godot::String& path, bool compress = true);
    static bool compress_existing_file(const godot::String& path);
    static bool uncompress_existing_file(const godot::String& path);
    void close();

    bool is_open() const { return is_open_flag; }
    bool fail() const { return file_failed; }
    uint64_t get_size() const { return is_streaming ? stream_size : buffer.size(); }

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

        if (is_streaming) {
            if (!stream_read(data, size)) file_failed = true;
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
