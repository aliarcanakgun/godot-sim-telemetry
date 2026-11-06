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

static godot::String globalize_if_needed(const godot::String& path) {
    godot::String os_path = path;
    if (os_path.begins_with("res://") || os_path.begins_with("user://")) {
        os_path = godot::ProjectSettings::get_singleton()->globalize_path(os_path);
    }
    return os_path;
}

bool TelemetryFile::open_read(const godot::String& path) {
    close();
    is_writing = false;
    rw_pos = 0;
    file_failed = false;

    godot::String os_path = globalize_if_needed(path);

    in_file.open(os_path.utf8().get_data(), std::ios::binary);
    if (!in_file.is_open()) {
        file_failed = true;
        return false;
    }

    in_file.seekg(0, std::ios::end);
    uint64_t file_size = static_cast<uint64_t>(in_file.tellg());
    in_file.seekg(0, std::ios::beg);

    if (file_size < 4) {
        close_stream();
        file_failed = true;
        return false;
    }

    in_file.read(reinterpret_cast<char*>(head), 4);
    if (!in_file) {
        close_stream();
        file_failed = true;
        return false;
    }

    stream_compressed = false;

    if (file_size >= COMPRESSED_HEADER_SIZE) {
        char magic[4];
        in_file.read(magic, 4);

        if (in_file && std::strncmp(magic, "ZST2", 4) == 0) { // compressed file
            uint64_t uncompressed_size = 0;
            in_file.read(reinterpret_cast<char*>(&uncompressed_size), 8);

            dctx = ZSTD_createDCtx();
            if (!in_file || !dctx) {
                close_stream();
                file_failed = true;
                return false;
            }

            in_buf.resize(ZSTD_DStreamInSize());
            scratch.resize(ZSTD_DStreamOutSize());
            in_buf_pos = 0;
            in_buf_size = 0;
            in_eof = false;

            stream_compressed = true;
            stream_size = 4 + uncompressed_size;
            stream_pos = 4;

            // integrity check
            // the frame header must agree with our header
            stream_refill();
            unsigned long long fcs = ZSTD_getFrameContentSize(in_buf.data(), in_buf_size);
            if (fcs == ZSTD_CONTENTSIZE_ERROR || (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != uncompressed_size)) {
                close_stream();
                file_failed = true;
                return false;
            }
        }
    }

    if (!stream_compressed) {
        // uncompressed file
        // real random access through the file itself
        in_file.clear();
        stream_size = file_size;
        stream_pos = UINT64_MAX; // force a seek on first read
    }

    is_streaming = true;
    is_open_flag = true;
    return true;
}

void TelemetryFile::close_stream() {
    if (dctx) {
        ZSTD_freeDCtx(dctx);
        dctx = nullptr;
    }
    if (in_file.is_open()) in_file.close();
    in_file.clear();

    std::vector<uint8_t>().swap(in_buf);
    std::vector<uint8_t>().swap(scratch);
    in_buf_pos = 0;
    in_buf_size = 0;
    in_eof = false;

    is_streaming = false;
    stream_compressed = false;
    stream_size = 0;
    stream_pos = 0;
}

bool TelemetryFile::stream_refill() {
    in_buf_pos = 0;
    in_buf_size = 0;
    if (in_eof) return false;

    in_file.read(reinterpret_cast<char*>(in_buf.data()), in_buf.size());
    in_buf_size = static_cast<size_t>(in_file.gcount());
    if (in_buf_size == 0) {
        in_eof = true;
        return false;
    }
    return true;
}

bool TelemetryFile::stream_rewind() {
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    in_file.clear();
    in_file.seekg(COMPRESSED_HEADER_SIZE, std::ios::beg);
    if (!in_file) return false;

    in_buf_pos = 0;
    in_buf_size = 0;
    in_eof = false;
    stream_pos = 4;
    return true;
}

bool TelemetryFile::stream_decompress(uint8_t* dst, uint64_t size) {
    ZSTD_outBuffer out = { dst, static_cast<size_t>(size), 0 };

    while (out.pos < out.size) {
        if (in_buf_pos >= in_buf_size) stream_refill();

        ZSTD_inBuffer in = { in_buf.data(), in_buf_size, in_buf_pos };
        size_t out_before = out.pos;
        size_t in_before = in.pos;

        size_t ret = ZSTD_decompressStream(dctx, &out, &in);
        in_buf_pos = in.pos;

        if (ZSTD_isError(ret)) return false;

        if (out.pos < out.size) {
            // corrupted
            if (ret == 0) return false;
            // no progress possible
            if (out.pos == out_before && in.pos == in_before && in_eof) return false;
        }
    }

    stream_pos += out.pos;
    return true;
}

bool TelemetryFile::stream_skip(uint64_t size) {
    while (size > 0) {
        uint64_t chunk = size < scratch.size() ? size : scratch.size();
        if (!stream_decompress(scratch.data(), chunk)) return false;
        size -= chunk;
    }
    return true;
}

bool TelemetryFile::stream_read(void* dst, uint64_t size) {
    if (size == 0) return true;
    if (rw_pos > stream_size || size > stream_size - rw_pos) return false;

    uint8_t* out = static_cast<uint8_t*>(dst);

    if (!stream_compressed) {
        if (stream_pos != rw_pos) {
            in_file.clear();
            in_file.seekg(static_cast<std::streamoff>(rw_pos), std::ios::beg);
            if (!in_file) return false;
        }
        in_file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
        if (static_cast<uint64_t>(in_file.gcount()) != size) {
            stream_pos = UINT64_MAX;
            return false;
        }
        rw_pos += size;
        stream_pos = rw_pos;
        return true;
    }

    // first 4 bytes are stored raw
    if (rw_pos < 4) {
        uint64_t n = 4 - rw_pos;
        if (n > size) n = size;
        std::memcpy(out, head + rw_pos, n);
        out += n;
        size -= n;
        rw_pos += n;
        if (size == 0) return true;
    }

    if (rw_pos < stream_pos) {
        if (!stream_rewind()) return false;
    }
    if (rw_pos > stream_pos) {
        if (!stream_skip(rw_pos - stream_pos)) return false;
    }

    if (!stream_decompress(out, size)) return false;
    rw_pos += size;
    return true;
}

// loads the whole file into memory.
// only used by compress/uncompress helpers,
// which need the full payload anyway.
bool TelemetryFile::open_read_full(const godot::String& path) {
    close();
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
    if (!file.open_read_full(path)) return false;
    
    file.is_writing = true; // allow close_and_save to work
    file.is_open_flag = true;
    return file.close_and_save(path, true);
}

bool TelemetryFile::uncompress_existing_file(const godot::String& path) {
    TelemetryFile file;
    if (!file.open_read_full(path)) return false;
    
    file.is_writing = true; // allow close_and_save to work
    file.is_open_flag = true;
    return file.close_and_save(path, false);
}

void TelemetryFile::close() {
    is_open_flag = false;
    buffer.clear();
    rw_pos = 0;
    close_stream();
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
