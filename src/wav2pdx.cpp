/**
 * wav2pdx — X68000 PCM8++ 兼容 PDX 编译器
 *
 * 将音频文件编译为 MDX/MXDRV 使用的 PDX 格式。
 * 支持 WAV 直接输入，其他格式（MP3/FLAC/OGG/AIFF/AAC 等）通过 ffmpeg 自动转码。
 * 输出支持 ADPCM (MSM6258)、16-bit PCM、8-bit PCM，单声道/立体声。
 *
 * 参照:
 *   - MDXWin CPDXFile.cs / CPDXCommon.cs
 *   - PCM8++ マーキュリー用PCMドライバー ver0.83d
 *   - MAME okim6258 ADPCM 实现
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

// ============================================================================
//  常量定义
// ============================================================================

// PDX 头表: 96 个槽 × 8 字节 = 768 (0x300) 字节
static const int PDX_NUM_SLOTS     = 96;
static const int PDX_HEADER_SIZE   = PDX_NUM_SLOTS * 8;  // 0x300

// ============================================================================
//  Big-Endian 工具函数
// ============================================================================

/// 将 32 位整数写入 Big-Endian 字节
static void write_be32(uint8_t* dst, int32_t val) {
    dst[0] = (uint8_t)((val >> 24) & 0xFF);
    dst[1] = (uint8_t)((val >> 16) & 0xFF);
    dst[2] = (uint8_t)((val >>  8) & 0xFF);
    dst[3] = (uint8_t)((val      ) & 0xFF);
}

/// 将 16 位整数写入 Big-Endian 字节
static void write_be16(uint8_t* dst, int16_t val) {
    dst[0] = (uint8_t)((val >> 8) & 0xFF);
    dst[1] = (uint8_t)((val     ) & 0xFF);
}

/// 读取 Little-Endian 16 位无符号
static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/// 读取 Little-Endian 32 位无符号
static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// ============================================================================
//  WAV 文件解析器
// ============================================================================

struct WavData {
    int sample_rate;      // 采样率
    int bits_per_sample;  // 位深度
    int num_channels;     // 声道数
    std::vector<int16_t> samples;  // 统一归一化为 16-bit signed 的采样数据

    /// 从文件加载 WAV
    bool load(const char* filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
            return false;
        }

        // 读取完整文件
        file.seekg(0, std::ios::end);
        size_t file_size = (size_t)file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buf(file_size);
        file.read(reinterpret_cast<char*>(buf.data()), file_size);
        file.close();

        if (file_size < 44) {
            fprintf(stderr, "Error: File too small, not a valid WAV file\n");
            return false;
        }

        // 验证 RIFF 头
        if (memcmp(buf.data(), "RIFF", 4) != 0 ||
            memcmp(buf.data() + 8, "WAVE", 4) != 0) {
            fprintf(stderr, "Error: Not a valid RIFF/WAVE file\n");
            return false;
        }

        // 查找 fmt 和 data chunk
        const uint8_t* fmt_data = nullptr;
        uint32_t fmt_chunk_size = 0;
        const uint8_t* pcm_data = nullptr;
        uint32_t pcm_data_size = 0;

        size_t pos = 12;  // 跳过 RIFF 头
        while (pos + 8 <= file_size) {
            uint32_t chunk_size = read_le32(buf.data() + pos + 4);
            if (memcmp(buf.data() + pos, "fmt ", 4) == 0) {
                fmt_data = buf.data() + pos + 8;
                fmt_chunk_size = chunk_size;
            } else if (memcmp(buf.data() + pos, "data", 4) == 0) {
                pcm_data = buf.data() + pos + 8;
                pcm_data_size = chunk_size;
            }
            pos += 8 + chunk_size;
            // chunk 大小对齐到偶数
            if (chunk_size & 1) pos++;
        }

        if (!fmt_data || !pcm_data) {
            fprintf(stderr, "Error: WAV file missing fmt or data chunk\n");
            return false;
        }

        // 解析 fmt chunk
        uint16_t audio_format = read_le16(fmt_data);
        num_channels    = read_le16(fmt_data + 2);
        sample_rate     = (int)read_le32(fmt_data + 4);
        bits_per_sample = read_le16(fmt_data + 14);

        // WAVE_FORMAT_EXTENSIBLE (0xFFFE): 从 SubFormat GUID 提取真实格式码
        if (audio_format == 0xFFFE) {
            // fmt chunk 至少需要 40 字节 (标准 18 + cbSize 里的 22 字节扩展)
            if (fmt_chunk_size < 40) {
                fprintf(stderr, "Error: EXTENSIBLE fmt chunk too short (%u bytes)\n", fmt_chunk_size);
                return false;
            }
            // SubFormat GUID 位于 fmt_data + 24，前 2 字节是真实格式码
            audio_format = read_le16(fmt_data + 24);
            // 注意: bits_per_sample 保持为容器大小（决定字节步长），
            // wValidBitsPerSample 仅影响数据解释精度，不影响读取步长。
            // 例如 24-in-32: 容器=32bit(步长4字节), 有效=24bit，
            // 取高16位的逻辑对 32bit 容器依然正确。
        }

        // 支持 PCM (1) 和 IEEE Float (3)
        bool is_float = (audio_format == 3);
        if (audio_format != 1 && audio_format != 3) {
            fprintf(stderr, "Error: Unsupported WAV format (format=%d, only PCM and IEEE Float supported)\n", audio_format);
            return false;
        }

        if (num_channels < 1 || num_channels > 2) {
            fprintf(stderr, "Error: Only mono or stereo supported (channels=%d)\n", num_channels);
            return false;
        }

        // 将原始 PCM 数据归一化为 int16_t 数组
        int bytes_per_sample = (is_float && bits_per_sample == 32) ? 4 :
                               (is_float && bits_per_sample == 64) ? 8 :
                               bits_per_sample / 8;
        int total_samples = pcm_data_size / bytes_per_sample;
        samples.resize(total_samples);

        if (is_float) {
            // IEEE Float → int16_t
            for (int i = 0; i < total_samples; i++) {
                const uint8_t* sp = pcm_data + i * bytes_per_sample;
                double val;
                if (bytes_per_sample == 4) {
                    // 32-bit float LE
                    float f;
                    memcpy(&f, sp, 4);
                    val = (double)f;
                } else if (bytes_per_sample == 8) {
                    // 64-bit double LE
                    memcpy(&val, sp, 8);
                } else {
                    fprintf(stderr, "Error: Unsupported %d-bit float WAV\n", bits_per_sample);
                    return false;
                }
                // 钳位到 [-1.0, 1.0] 并缩放到 int16
                if (val > 1.0) val = 1.0;
                if (val < -1.0) val = -1.0;
                samples[i] = (int16_t)(val * 32767.0);
            }
        } else {
            // 整数 PCM → int16_t
            for (int i = 0; i < total_samples; i++) {
                const uint8_t* sp = pcm_data + i * bytes_per_sample;
                switch (bits_per_sample) {
                    case 8:
                        // 8-bit WAV 是 unsigned (0-255)，中心值 128
                        samples[i] = (int16_t)((sp[0] - 128) << 8);
                        break;
                    case 16:
                        // 16-bit WAV 是 signed LE
                        samples[i] = (int16_t)(sp[0] | (sp[1] << 8));
                        break;
                    case 24:
                        // 24-bit WAV 是 signed LE，取高 16 位
                        samples[i] = (int16_t)((sp[1] | (sp[2] << 8)));
                        break;
                    case 32:
                        // 32-bit WAV 是 signed LE，取高 16 位
                        samples[i] = (int16_t)((sp[2] | (sp[3] << 8)));
                        break;
                    default:
                        fprintf(stderr, "Error: Unsupported %d-bit WAV\n", bits_per_sample);
                        return false;
                }
            }
        }

        return true;
    }

    /// 获取单声道采样数（如果是立体声，返回每声道的采样数）
    int num_frames() const {
        return (int)(samples.size() / num_channels);
    }

    /// 获取指定声道的单声道采样
    std::vector<int16_t> get_channel(int ch) const {
        std::vector<int16_t> result;
        int frames = num_frames();
        result.reserve(frames);
        for (int i = 0; i < frames; i++) {
            result.push_back(samples[i * num_channels + ch]);
        }
        return result;
    }

    /// 将立体声混音为单声道
    std::vector<int16_t> to_mono() const {
        if (num_channels == 1) return samples;
        int frames = num_frames();
        std::vector<int16_t> result(frames);
        for (int i = 0; i < frames; i++) {
            int32_t mix = (int32_t)samples[i * 2] + (int32_t)samples[i * 2 + 1];
            result[i] = (int16_t)(mix / 2);
        }
        return result;
    }
};

// ============================================================================
//  采样率转换器（线性插值）
// ============================================================================

/// 将采样从 src_rate 重采样到 dst_rate
static std::vector<int16_t> resample(const std::vector<int16_t>& input,
                                      int src_rate, int dst_rate) {
    if (src_rate == dst_rate) return input;
    if (input.empty()) return {};

    int src_len = (int)input.size();
    int dst_len = (int)((int64_t)src_len * dst_rate / src_rate);
    if (dst_len <= 0) return {};

    std::vector<int16_t> output(dst_len);
    double ratio = (double)src_rate / (double)dst_rate;

    for (int i = 0; i < dst_len; i++) {
        double src_pos = i * ratio;
        int idx = (int)src_pos;
        double frac = src_pos - idx;

        int16_t s0 = input[std::min(idx, src_len - 1)];
        int16_t s1 = input[std::min(idx + 1, src_len - 1)];
        double val = s0 * (1.0 - frac) + s1 * frac;

        // 钳位到 int16_t 范围
        if (val > 32767.0)  val = 32767.0;
        if (val < -32768.0) val = -32768.0;
        output[i] = (int16_t)val;
    }

    return output;
}

// ============================================================================
//  MSM6258 (OKI ADPCM) 编码器
// ============================================================================

/// MAME 风格 diff_lookup 预计算表
static int adpcm_diff_lookup[49 * 16];
static bool adpcm_tables_computed = false;

/// 初始化 ADPCM 查找表
static void compute_adpcm_tables() {
    if (adpcm_tables_computed) return;

    // 步长表（49 个步长值）
    static const int step_table[49] = {
         16,  17,  19,  21,  23,  25,  28,  31,  34,  37,
         41,  45,  50,  55,  60,  66,  73,  80,  88,  97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
        279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963,1060,1166,1282,1411,1552
    };

    for (int step = 0; step < 49; step++) {
        for (int nib = 0; nib < 16; nib++) {
            int value = step_table[step] / 8;
            if (nib & 1) value += step_table[step] / 4;
            if (nib & 2) value += step_table[step] / 2;
            if (nib & 4) value += step_table[step];
            if (nib & 8) value = -value;
            adpcm_diff_lookup[step * 16 + nib] = value;
        }
    }
    adpcm_tables_computed = true;
}

/// ADPCM 步长索引调整表
static const int adpcm_index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/// ADPCM 编码器状态
struct AdpcmEncoder {
    int signal;       // 当前信号值
    int step_index;   // 当前步长索引

    AdpcmEncoder() : signal(0), step_index(0) {}

    /// 编码一个采样为 4-bit nibble
    uint8_t encode_sample(int16_t sample) {
        // 解码器: Pcm 在 ~12-bit 域累加，输出 InpPcm = Pcm << 4
        // 编码器: signal 使用同一 diff_lookup 在 ~12-bit 域累加
        // 因此输入须从 16-bit 缩放到 12-bit (右移 4 位)
        int target = (int)sample >> 4;

        // 选择最佳 nibble：遍历所有 16 个可能值，找最小误差
        int best_nibble = 0;
        int best_error = 0x7FFFFFFF;

        for (int nib = 0; nib < 16; nib++) {
            int predicted = signal + adpcm_diff_lookup[step_index * 16 + nib];
            int error = abs(target - predicted);
            if (error < best_error) {
                best_error = error;
                best_nibble = nib;
            }
        }

        // 更新信号值
        signal += adpcm_diff_lookup[step_index * 16 + best_nibble];

        // 更新步长索引
        step_index += adpcm_index_shift[best_nibble & 7];
        if (step_index < 0)  step_index = 0;
        if (step_index > 48) step_index = 48;

        return (uint8_t)best_nibble;
    }

    /// 将 int16_t 采样数组编码为 ADPCM 字节流
    /// X68000 PCM8 打包方式: 低 nibble 先（第 1 个采样），高 nibble 后（第 2 个采样）
    /// 解码器先取 byte & 0x0F（低 nibble），再取 byte >> 4（高 nibble）
    std::vector<uint8_t> encode(const std::vector<int16_t>& samples) {
        compute_adpcm_tables();

        int num_samples = (int)samples.size();
        // 每两个采样生成一个字节
        int num_bytes = (num_samples + 1) / 2;
        std::vector<uint8_t> result(num_bytes, 0);

        for (int i = 0; i < num_samples; i++) {
            uint8_t nibble = encode_sample(samples[i]);
            if (i % 2 == 0) {
                // 第 1 个采样 → 低 nibble（解码器先读）
                result[i / 2] = (nibble & 0x0F);
            } else {
                // 第 2 个采样 → 高 nibble（解码器后读）
                result[i / 2] |= (nibble << 4);
            }
        }

        return result;
    }
};

// ============================================================================
//  PDX 文件构建器
// ============================================================================

/// 一个 PDX voice 槽
struct PdxSlot {
    int bank;                      // bank 编号 (0-based)
    int slot_index;                // 0-95 的槽位号（bank 内）
    std::vector<uint8_t> data;     // 编码后的音频数据
    int sample_rate;               // 原始采样率 (Hz)，仅可变频率模式非零

    PdxSlot() : bank(0), slot_index(0), sample_rate(0) {}
};

/// 构建多 bank PDX 文件并写入磁盘
/// EX-PDX 格式: [Bank0 头表][Bank1 头表]...[所有音频数据]
static bool write_pdx(const char* filename, const std::vector<PdxSlot>& slots) {
    // 确定 bank 数量
    int num_banks = 1;
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].bank + 1 > num_banks) {
            num_banks = slots[i].bank + 1;
        }
    }

    int total_header_size = num_banks * PDX_HEADER_SIZE;

    // 按 bank 再按 slot_index 排序
    std::vector<PdxSlot> sorted_slots = slots;
    std::sort(sorted_slots.begin(), sorted_slots.end(),
              [](const PdxSlot& a, const PdxSlot& b) {
                  if (a.bank != b.bank) return a.bank < b.bank;
                  return a.slot_index < b.slot_index;
              });

    // 为每个 bank 构建偏移/长度表
    // offsets[bank][slot] / lengths[bank][slot]
    std::vector<std::vector<int>> offsets(num_banks, std::vector<int>(PDX_NUM_SLOTS, 0));
    std::vector<std::vector<int>> lengths(num_banks, std::vector<int>(PDX_NUM_SLOTS, 0));

    int data_offset = total_header_size;  // 数据从所有头表之后开始

    for (size_t i = 0; i < sorted_slots.size(); i++) {
        int b = sorted_slots[i].bank;
        int idx = sorted_slots[i].slot_index;
        if (idx < 0 || idx >= PDX_NUM_SLOTS) {
            fprintf(stderr, "Error: bank %d slot %d out of range (0-%d)\n",
                    b, idx, PDX_NUM_SLOTS - 1);
            return false;
        }
        offsets[b][idx] = data_offset;
        lengths[b][idx] = (int)sorted_slots[i].data.size();
        data_offset += (int)sorted_slots[i].data.size();
    }

    // 写入文件
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Cannot create output file '%s'\n", filename);
        return false;
    }

    // 写入所有 bank 的头表
    for (int b = 0; b < num_banks; b++) {
        uint8_t header[PDX_HEADER_SIZE];
        memset(header, 0, sizeof(header));
        for (int i = 0; i < PDX_NUM_SLOTS; i++) {
            write_be32(header + i * 8,     offsets[b][i]);
            write_be32(header + i * 8 + 4, lengths[b][i]);
        }
        file.write(reinterpret_cast<const char*>(header), PDX_HEADER_SIZE);
    }

    // 写入所有音频数据
    for (size_t i = 0; i < sorted_slots.size(); i++) {
        file.write(reinterpret_cast<const char*>(sorted_slots[i].data.data()),
                   sorted_slots[i].data.size());
    }

    // ── 追加采样率元数据（仅当存在可变频率槽时）──
    // 格式: "PDXr" + version(2B) + count(2B) + entries(N×6B) + "rXDP"
    {
        // 收集可变频率槽
        std::vector<std::pair<int, int>> rate_entries;  // (slot_index, sample_rate)
        for (size_t i = 0; i < slots.size(); i++) {
            if (slots[i].sample_rate > 0) {
                rate_entries.push_back(std::make_pair(slots[i].slot_index, slots[i].sample_rate));
            }
        }

        if (!rate_entries.empty()) {
            // Magic: "PDXr"
            file.write("PDXr", 4);

            // Version: 0x0001 (Big-Endian)
            uint8_t ver[2] = { 0x00, 0x01 };
            file.write(reinterpret_cast<const char*>(ver), 2);

            // Num Entries (Big-Endian)
            uint8_t cnt[2];
            write_be16((uint8_t*)cnt, (int16_t)rate_entries.size());
            file.write(reinterpret_cast<const char*>(cnt), 2);

            // Entries: slot_index(2B) + sample_rate(4B)，均为 Big-Endian
            for (size_t i = 0; i < rate_entries.size(); i++) {
                uint8_t entry[6];
                write_be16(entry, (int16_t)rate_entries[i].first);
                write_be32(entry + 2, rate_entries[i].second);
                file.write(reinterpret_cast<const char*>(entry), 6);
            }

            // Tail Magic: "rXDP"（从文件尾快速定位用）
            file.write("rXDP", 4);

            printf("  Sample rate metadata: %zu variable-frequency slots\n", rate_entries.size());
        }
    }

    if (num_banks > 1) {
        printf("  PDX format: EX-PDX (%d banks)\n", num_banks);
    }

    file.close();
    return true;
}

// ============================================================================
//  音频格式转换
// ============================================================================

/// 将 int16_t 采样转换为 Big-Endian 16-bit PCM 字节流
static std::vector<uint8_t> encode_pcm16(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> result(samples.size() * 2);
    for (size_t i = 0; i < samples.size(); i++) {
        write_be16(result.data() + i * 2, samples[i]);
    }
    return result;
}

/// 将 int16_t 采样转换为 signed 8-bit PCM 字节流
static std::vector<uint8_t> encode_pcm8(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> result(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        // int16 → int8：取高 8 位
        result[i] = (uint8_t)((samples[i] >> 8) & 0xFF);
    }
    return result;
}

/// 将立体声 int16_t 交错采样转换为 Big-Endian 16-bit PCM 字节流
static std::vector<uint8_t> encode_pcm16_stereo(const std::vector<int16_t>& interleaved) {
    std::vector<uint8_t> result(interleaved.size() * 2);
    for (size_t i = 0; i < interleaved.size(); i++) {
        write_be16(result.data() + i * 2, interleaved[i]);
    }
    return result;
}

/// 将立体声 int16_t 交错采样转换为 signed 8-bit PCM 字节流
static std::vector<uint8_t> encode_pcm8_stereo(const std::vector<int16_t>& interleaved) {
    std::vector<uint8_t> result(interleaved.size());
    for (size_t i = 0; i < interleaved.size(); i++) {
        result[i] = (uint8_t)((interleaved[i] >> 8) & 0xFF);
    }
    return result;
}

// ============================================================================
//  F 值模式码解析系统
// ============================================================================

/// PCM 驱动类型
enum PcmDriver {
    DRIVER_NONE   = 0,  // 纯 ADPCM（标准 MXDRV，无驱动扩展）
    DRIVER_PCM8A  = 1,  // PCM8A（philly 1993-1997）
    DRIVER_PCM8PP = 2,  // PCM8++（たにぃ 1994-1996）
};

/// F 值解析结果：编码参数
struct FModeInfo {
    const char* format;   // "adpcm", "pcm16", "pcm8"
    int rate;             // 目标采样率 (Hz)
    bool stereo;          // 是否立体声
    int mode_code;        // 模式码（MDX 0xED 参数）
};

/// PCM8++ F 值频率表（F8-F14 / F16-F22 / F24-F30 / F32-F38）
static const int pcm8pp_fixed_rates[] = {
    15625, 16000, 22050, 24000, 32000, 44100, 48000
};

/// 解析 F 值 → 编码参数（根据驱动类型）
static bool resolve_f_mode(PcmDriver driver, int f_val, FModeInfo& info) {
    // F0-F4: 所有驱动通用的 ADPCM
    if (f_val >= 0 && f_val <= 4) {
        static const int adpcm_rates[] = { 3906, 5208, 7812, 10416, 15625 };
        info.format = "adpcm";
        info.rate = adpcm_rates[f_val];
        info.stereo = false;
        info.mode_code = f_val;
        return true;
    }

    // 以下需要驱动支持
    if (driver == DRIVER_NONE) {
        fprintf(stderr, "Error: F%d requires PCM driver (#ex-pcm 0 only supports F0-F4)\n", f_val);
        return false;
    }

    // ── PCM8A 驱动 ──
    if (driver == DRIVER_PCM8A) {
        // PCM8A 模式码表（来自 pcm8atec.doc）
        //   F5=$05 16bit 15.6kHz, F6=$06 8bit 15.6kHz
        //   F7=$07 ADPCM 20.8kHz, F8=$08 16bit 20.8kHz, F9=$09 8bit 20.8kHz
        //   F10=$0A ADPCM 31.2kHz, F11=$0B 16bit 31.2kHz, F12=$0C 8bit 31.2kHz
        struct Pcm8aMode { const char* fmt; int rate; int code; };
        static const Pcm8aMode pcm8a_modes[] = {
            // F5-F6: 15.6kHz PCM
            { "pcm16", 15625, 0x05 },  // F5
            { "pcm8",  15625, 0x06 },  // F6
            // F7-F9: 20.8kHz
            { "adpcm", 20833, 0x07 },  // F7
            { "pcm16", 20833, 0x08 },  // F8
            { "pcm8",  20833, 0x09 },  // F9
            // F10-F12: 31.2kHz
            { "adpcm", 31250, 0x0A },  // F10
            { "pcm16", 31250, 0x0B },  // F11
            { "pcm8",  31250, 0x0C },  // F12
        };
        int idx = f_val - 5;
        if (idx < 0 || idx >= 8) {
            fprintf(stderr, "Error: PCM8A does not support F%d (valid range: F0-F12)\n", f_val);
            return false;
        }
        info.format = pcm8a_modes[idx].fmt;
        info.rate = pcm8a_modes[idx].rate;
        info.stereo = false;  // PCM8A 不支持立体声
        info.mode_code = pcm8a_modes[idx].code;
        return true;
    }

    // ── PCM8++ 驱动 ──
    // F5=$05 16bit 15.6kHz (旧兼容), F6=$06 8bit 15.6kHz (旧兼容)
    if (f_val == 5) {
        info.format = "pcm16"; info.rate = 15625;
        info.stereo = false; info.mode_code = 0x05;
        return true;
    }
    if (f_val == 6) {
        info.format = "pcm8"; info.rate = 15625;
        info.stereo = false; info.mode_code = 0x06;
        return true;
    }
    // F7=$07 Through（频率由硬件决定，需用户通过 <hz> 指定目标采样率）
    if (f_val == 7) {
        info.format = "pcm16";
        info.rate = 0;          // 0 = 调用方必须提供 rate_override
        info.stereo = false;
        info.mode_code = 0x07;
        return true;
    }
    // F8-F14($0E): 16bit PCM mono (7 种固定频率)
    if (f_val >= 8 && f_val <= 14) {
        int idx = f_val - 8;
        info.format = "pcm16"; info.rate = pcm8pp_fixed_rates[idx];
        info.stereo = false; info.mode_code = f_val;
        return true;
    }
    // F15($0F): 可变频率 16bit mono（和 F7 相同，须指定 rate_override）
    if (f_val == 15) {
        info.format = "pcm16";
        info.rate = 0;          // 调用方必须提供 rate_override
        info.stereo = false;
        info.mode_code = 0x0F;
        return true;
    }
    // F16($10)-F22($16): 8bit PCM mono
    if (f_val >= 16 && f_val <= 22) {
        int idx = f_val - 16;
        info.format = "pcm8"; info.rate = pcm8pp_fixed_rates[idx];
        info.stereo = false; info.mode_code = f_val;
        return true;
    }
    // F23($17): 可变频率 8bit mono（须指定 rate_override）
    if (f_val == 23) {
        info.format = "pcm8";
        info.rate = 0;
        info.stereo = false;
        info.mode_code = 0x17;
        return true;
    }
    // F24($18)-F30($1E): 16bit PCM stereo
    if (f_val >= 24 && f_val <= 30) {
        int idx = f_val - 24;
        info.format = "pcm16"; info.rate = pcm8pp_fixed_rates[idx];
        info.stereo = true; info.mode_code = f_val;
        return true;
    }
    // F31($1F): 可变频率 16bit stereo（须指定 rate_override）
    if (f_val == 31) {
        info.format = "pcm16";
        info.rate = 0;
        info.stereo = true;
        info.mode_code = 0x1F;
        return true;
    }
    // F32($20)-F38($26): 8bit PCM stereo
    if (f_val >= 32 && f_val <= 38) {
        int idx = f_val - 32;
        info.format = "pcm8"; info.rate = pcm8pp_fixed_rates[idx];
        info.stereo = true; info.mode_code = f_val;
        return true;
    }

    // F39($27): 可变频率 8bit stereo（须指定 rate_override）
    if (f_val == 39) {
        info.format = "pcm8";
        info.rate = 0;
        info.stereo = true;
        info.mode_code = 0x27;
        return true;
    }
    // F40($28): 可变频率 ADPCM mono（须指定 rate_override）
    if (f_val == 40) {
        info.format = "adpcm";
        info.rate = 0;
        info.stereo = false;
        info.mode_code = 0x28;
        return true;
    }
    // F41($29): 可变频率 16bit PCM mono（须指定 rate_override）
    if (f_val == 41) {
        info.format = "pcm16";
        info.rate = 0;
        info.stereo = false;
        info.mode_code = 0x29;
        return true;
    }

    fprintf(stderr, "Error: PCM8++ does not support F%d (valid range: F0-F41)\n", f_val);
    return false;
}

// ============================================================================
//  前向声明（转码相关函数定义在后方）
// ============================================================================

static bool is_wav_file(const char* filename);
static bool transcode_to_wav(const char* input, std::string& out_wav_path);

// ============================================================================
//  处理流水线：WAV → PDX 槽数据
// ============================================================================

/// 音频处理参数
struct VoiceConfig {
    int bank;                 // bank 编号 (0-based)
    int slot_index;           // PDX 槽位号 (0-95)
    int f_mode;               // F 值（模式码）
    PcmDriver driver;         // 目标驱动
    int rate_override;        // 采样率覆盖（0=使用 F 值默认）
    int stereo_override;      // 声道覆盖（-1=不覆盖, 0=mono, 1=stereo）
    int volume;               // 音量百分比（100=不变, 50=减半, 200=加倍）
    std::string wav_filename; // 音频文件路径（WAV 直接加载，其他格式 ffmpeg 转码）
};

/// 对采样数据应用音量缩放（重采样后、编码前调用）
static void apply_volume(std::vector<int16_t>& samples, int volume_percent) {
    if (volume_percent == 100) return;  // 无变化
    double scale = volume_percent / 100.0;
    for (size_t i = 0; i < samples.size(); i++) {
        double val = samples[i] * scale;
        // 钳位到 int16_t 范围
        if (val > 32767.0)  val = 32767.0;
        if (val < -32768.0) val = -32768.0;
        samples[i] = (int16_t)val;
    }
}

/// 处理单个 voice 配置，生成 PDX 槽数据
static bool process_voice(const VoiceConfig& cfg, PdxSlot& slot) {
    // 解析 F 值 → 编码参数
    FModeInfo mode;
    if (!resolve_f_mode(cfg.driver, cfg.f_mode, mode)) {
        return false;
    }

    // 采样率覆盖（可变频率模式要求必须指定 rate_override）
    if (mode.rate == 0 && cfg.rate_override <= 0) {
        fprintf(stderr, "Error: F%d (variable frequency) requires explicit sample rate\n"
                        "       Example: 0=vocal.wav,22050\n", cfg.f_mode);
        return false;
    }
    int target_rate = (cfg.rate_override > 0) ? cfg.rate_override : mode.rate;

    // 声道覆盖
    if (cfg.stereo_override >= 0) {
        mode.stereo = (cfg.stereo_override == 1);
    }

    // ── 加载音频 ──
    // 非 WAV 文件通过 ffmpeg 转码为临时 WAV
    // WAV 文件先尝试原生加载，失败则回退 ffmpeg（处理 EXTENSIBLE 等非标准格式）
    std::string temp_wav_path;
    bool need_cleanup = false;
    const char* load_path = cfg.wav_filename.c_str();

    if (!is_wav_file(load_path)) {
        // 非 WAV 格式：直接走 ffmpeg
        if (!transcode_to_wav(load_path, temp_wav_path)) {
            return false;
        }
        load_path = temp_wav_path.c_str();
        need_cleanup = true;
    }

    WavData wav;
    bool load_ok = wav.load(load_path);

    // WAV 原生加载失败 → 回退 ffmpeg 转码（如 WAVE_FORMAT_EXTENSIBLE 等）
    if (!load_ok && !need_cleanup) {
        fprintf(stderr, "  Note: WAV native parse failed, trying ffmpeg transcode...\n");
        if (transcode_to_wav(cfg.wav_filename.c_str(), temp_wav_path)) {
            load_ok = wav.load(temp_wav_path.c_str());
            need_cleanup = true;
        }
    }

    // 无论加载成功与否，都要清理临时文件
    if (need_cleanup) {
        remove(temp_wav_path.c_str());
    }

    if (!load_ok) {
        return false;
    }

    slot.bank = cfg.bank;
    slot.slot_index = cfg.slot_index;
    // 可变频率模式时记录原始采样率（用于 PDX 元数据）
    if (mode.rate == 0) {
        slot.sample_rate = target_rate;
    }

    if (strcmp(mode.format, "adpcm") == 0) {
        // ADPCM 编码
        std::vector<int16_t> mono = wav.to_mono();
        std::vector<int16_t> resampled = resample(mono, wav.sample_rate, target_rate);
        apply_volume(resampled, cfg.volume);
        AdpcmEncoder encoder;
        slot.data = encoder.encode(resampled);

        printf("  bank %d slot %2d: ADPCM @ %d Hz, %zu bytes (F%d, mode=0x%02X)\n",
               cfg.bank, cfg.slot_index, target_rate, slot.data.size(),
               cfg.f_mode, mode.mode_code);

    } else if (strcmp(mode.format, "pcm16") == 0) {
        if (mode.stereo && wav.num_channels == 2) {
            std::vector<int16_t> left  = resample(wav.get_channel(0), wav.sample_rate, target_rate);
            std::vector<int16_t> right = resample(wav.get_channel(1), wav.sample_rate, target_rate);
            apply_volume(left, cfg.volume);
            apply_volume(right, cfg.volume);
            int frames = (int)std::min(left.size(), right.size());
            std::vector<int16_t> interleaved(frames * 2);
            for (int i = 0; i < frames; i++) {
                interleaved[i * 2]     = left[i];
                interleaved[i * 2 + 1] = right[i];
            }
            slot.data = encode_pcm16_stereo(interleaved);
        } else {
            std::vector<int16_t> mono = wav.to_mono();
            std::vector<int16_t> resampled = resample(mono, wav.sample_rate, target_rate);
            apply_volume(resampled, cfg.volume);
            slot.data = encode_pcm16(resampled);
        }

        printf("  bank %d slot %2d: PCM16 %s @ %d Hz, %zu bytes (F%d, mode=0x%02X)\n",
               cfg.bank, cfg.slot_index, mode.stereo ? "stereo" : "mono",
               target_rate, slot.data.size(), cfg.f_mode, mode.mode_code);

    } else if (strcmp(mode.format, "pcm8") == 0) {
        if (mode.stereo && wav.num_channels == 2) {
            std::vector<int16_t> left  = resample(wav.get_channel(0), wav.sample_rate, target_rate);
            std::vector<int16_t> right = resample(wav.get_channel(1), wav.sample_rate, target_rate);
            apply_volume(left, cfg.volume);
            apply_volume(right, cfg.volume);
            int frames = (int)std::min(left.size(), right.size());
            std::vector<int16_t> interleaved(frames * 2);
            for (int i = 0; i < frames; i++) {
                interleaved[i * 2]     = left[i];
                interleaved[i * 2 + 1] = right[i];
            }
            slot.data = encode_pcm8_stereo(interleaved);
        } else {
            std::vector<int16_t> mono = wav.to_mono();
            std::vector<int16_t> resampled = resample(mono, wav.sample_rate, target_rate);
            apply_volume(resampled, cfg.volume);
            slot.data = encode_pcm8(resampled);
        }

        printf("  bank %d slot %2d: PCM8 %s @ %d Hz, %zu bytes (F%d, mode=0x%02X)\n",
               cfg.bank, cfg.slot_index, mode.stereo ? "stereo" : "mono",
               target_rate, slot.data.size(), cfg.f_mode, mode.mode_code);

    } else {
        fprintf(stderr, "Error: Internal error, unknown format '%s'\n", mode.format);
        return false;
    }

    return true;
}

// ============================================================================
//  文件格式检测与转码
// ============================================================================

/// 检查文件扩展名是否为 .wav（大小写不敏感）
static bool is_wav_file(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    if (strlen(ext) != 4) return false;
    return (tolower((unsigned char)ext[1]) == 'w' &&
            tolower((unsigned char)ext[2]) == 'a' &&
            tolower((unsigned char)ext[3]) == 'v');
}

/// 检查文件扩展名是否为 .pcm（原始 ADPCM 数据）
static bool is_pcm_file(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (tolower((unsigned char)ext[1]) == 'p' &&
            tolower((unsigned char)ext[2]) == 'c' &&
            tolower((unsigned char)ext[3]) == 'm' &&
            ext[4] == '\0');
}

/// 通过 ffmpeg 将任意音频文件转码为 16-bit PCM WAV
/// 成功时 out_wav_path 为临时 WAV 路径（调用方负责删除）
static bool transcode_to_wav(const char* input, std::string& out_wav_path) {
    // 生成临时文件路径
    out_wav_path = std::string(input) + ".tmp.wav";

    // 构建 ffmpeg 命令:
    //   -y: 覆盖输出
    //   -i input: 输入文件
    //   -acodec pcm_s16le: 16-bit signed LE PCM
    //   -f wav: WAV 容器
    //   -loglevel error: 仅输出错误
    std::string cmd = "ffmpeg -y -loglevel error -i \"" + std::string(input)
                    + "\" -acodec pcm_s16le -f wav \"" + out_wav_path + "\"";

    printf("  Transcoding: %s -> temp WAV ...\n", input);
    int ret = system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "Error: ffmpeg transcode failed (exit=%d)\n"
                        "       Command: %s\n"
                        "       Please ensure ffmpeg is installed and in PATH\n", ret, cmd.c_str());
        // 清理可能的残留文件
        remove(out_wav_path.c_str());
        return false;
    }
    return true;
}

// ============================================================================
//  清单文件解析（统一 PDL 格式）
// ============================================================================

/// 加载原始 PCM/ADPCM 数据文件
static bool load_raw_pcm(const char* filename, std::vector<uint8_t>& data) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return false;
    }
    file.seekg(0, std::ios::end);
    size_t size = (size_t)file.tellg();
    file.seekg(0, std::ios::beg);
    data.resize(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();
    return true;
}

/// 解析清单文件（MML 风格 PDL 格式）
///
/// 注释:
///   ;                  — 行注释（; 之后到行尾为注释）
///   /* ... */           — 块注释（可跨行）
///   # 注释             — 注释行（跳过）
///
/// 顶级指令（以 # 开头）:
///   #ex-pcm <0|1|2>   — 驱动: 0=纯ADPCM, 1=PCM8A, 2=PCM8++
///   #ex-pdx <n>        — EX-PDX bank 数声明（信息行）
///   #mode <f_val>      — 全局默认 F 值（未遇到 F 指令时使用）
///
/// bank/mode 选择器（独占一行）:
///   F<n>@<bank>        — 设置当前 F 模式 + bank（@bank 可省略）
///   @<bank>            — 仅切换 bank（保留当前 F 模式）
///
/// 采样条目:
///   N=filename          — .pcm 直接加载，.wav 按当前 F 模式编码
///   N=filename <hz>     — 可选：覆盖目标采样率（仅用于重采样）
static bool parse_manifest(const char* filename, std::vector<VoiceConfig>& configs,
                            std::vector<PdxSlot>& raw_slots,
                            PcmDriver& out_driver) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Cannot open manifest file '%s'\n", filename);
        return false;
    }

    // 提取清单文件所在目录（用于解析素材的相对路径）
    std::string manifest_dir;
    {
        std::string mpath(filename);
        size_t last_sep = mpath.find_last_of("/\\");
        if (last_sep != std::string::npos) {
            manifest_dir = mpath.substr(0, last_sep + 1);
        }
    }

    std::string line;
    int current_bank  = 0;
    int global_f      = 4;             // #mode 设定的全局默认 F 值
    int current_f     = 4;             // 当前 F 值（F<n> 临时覆盖，@N 恢复到 global_f）
    PcmDriver driver  = DRIVER_PCM8PP; // 默认 PCM8++
    int line_num = 0;
    bool in_block_comment = false;  // /* */ 块注释状态

    while (std::getline(file, line)) {
        line_num++;

        // ── 块注释处理（/* ... */，可跨行）──────────────
        if (in_block_comment) {
            size_t close = line.find("*/");
            if (close == std::string::npos) continue;  // 整行在块注释内，跳过
            line = line.substr(close + 2);  // 取 */ 之后的内容
            in_block_comment = false;
        }

        // 处理行内块注释（同一行内的 /* ... */）和未闭合的块注释开始
        for (;;) {
            size_t open = line.find("/*");
            if (open == std::string::npos) break;
            size_t close = line.find("*/", open + 2);
            if (close != std::string::npos) {
                // 同行闭合：移除 /* ... */ 部分
                line = line.substr(0, open) + line.substr(close + 2);
            } else {
                // 块注释开始，截断到 /* 之前
                line = line.substr(0, open);
                in_block_comment = true;
                break;
            }
        }

        // ── 行注释处理（; 到行尾）──────────────────────
        {
            size_t semi = line.find(';');
            if (semi != std::string::npos) {
                line = line.substr(0, semi);
            }
        }

        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);
        if (line.empty()) continue;

        // ── 顶级指令 (#...) ──────────────────────────────
        if (line[0] == '#') {
            std::string lower = line;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);

            if (lower.find("#ex-pcm ") == 0) {
                int val = atoi(line.c_str() + 8);
                if (val < 0 || val > 2) {
                    fprintf(stderr, "Error: line %d, invalid #ex-pcm value (0-2): %d\n", line_num, val);
                    return false;
                }
                driver = (PcmDriver)val;
                printf("Driver: %s\n",
                       driver == DRIVER_NONE ? "ADPCM only" :
                       driver == DRIVER_PCM8A ? "PCM8A" : "PCM8++");
            } else if (lower.find("#mode ") == 0) {
                global_f = atoi(line.c_str() + 6);
                current_f = global_f;
                printf("Global mode: F%d\n", global_f);
            }
            // #ex-pdx 和其他 # 开头行为注释，忽略
            continue;
        }

        // ── bank/mode 选择器 ─────────────────────────────
        // 格式: F<n>[@<bank>]  或  @<bank>
        // @<bank> 仅切换 bank，F 值恢复到 #mode 全局默认
        // F<n>[@<bank>] 显式覆盖 F 值
        if (line[0] == 'F' || line[0] == 'f' || line[0] == '@') {
            const char* p = line.c_str();
            int new_f    = global_f;  // 默认恢复到全局 F 值
            int new_bank = current_bank;

            if (*p == 'F' || *p == 'f') {
                p++;
                new_f = (int)strtol(p, const_cast<char**>(&p), 10);
            }
            if (*p == '@') {
                p++;
                new_bank = (int)strtol(p, nullptr, 10);
            }
            current_f    = new_f;
            current_bank = new_bank;
            printf("Mode switch: F%d @ bank %d\n", current_f, current_bank);
            continue;
        }

        // ── 采样条目: N=filename[,hz][,stereo|mono] ──────────
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            fprintf(stderr, "Error: line %d, bad format, missing '=': '%s'\n",
                    line_num, line.c_str());
            return false;
        }

        int slot_index = atoi(line.substr(0, eq_pos).c_str());
        if (slot_index < 0 || slot_index >= PDX_NUM_SLOTS) {
            fprintf(stderr, "Error: line %d, slot %d out of range (0-%d)\n",
                    line_num, slot_index, PDX_NUM_SLOTS - 1);
            return false;
        }

        // 逗号分隔解析: filename[,hz][,stereo|mono]
        std::string rhs = line.substr(eq_pos + 1);
        std::string entry_filename;
        int  rate_override = 0;
        int  stereo_override = -1;  // -1=不覆盖, 0=mono, 1=stereo
        int  volume = 100;          // 音量百分比（默认100=不变）

        // 按逗号拆分
        std::vector<std::string> tokens;
        {
            std::istringstream ss(rhs);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                // 去除首尾空格
                size_t s = tok.find_first_not_of(" \t");
                size_t e = tok.find_last_not_of(" \t");
                if (s != std::string::npos)
                    tokens.push_back(tok.substr(s, e - s + 1));
            }
        }

        if (tokens.empty() || tokens[0].empty()) {
            fprintf(stderr, "Error: line %d, missing filename\n", line_num);
            return false;
        }

        entry_filename = tokens[0];

        // 如果素材路径是相对路径，则拼接清单文件所在目录
        if (!manifest_dir.empty() && !entry_filename.empty()
            && entry_filename[0] != '/' && entry_filename[0] != '\\'
            && !(entry_filename.size() >= 2 && entry_filename[1] == ':')) {
            entry_filename = manifest_dir + entry_filename;
        }

        // 解析可选参数（顺序无关）
        for (size_t ti = 1; ti < tokens.size(); ti++) {
            const std::string &t = tokens[ti];
            if (t == "stereo" || t == "Stereo" || t == "STEREO") {
                stereo_override = 1;
            } else if (t == "mono" || t == "Mono" || t == "MONO") {
                stereo_override = 0;
            } else if ((t[0] == 'v' || t[0] == 'V') && t.size() > 1) {
                // 音量参数: v100 / v+50 / v-30
                char *endp = nullptr;
                long val = strtol(t.c_str() + 1, &endp, 10);
                if (endp != t.c_str() + 1 && *endp == '\0') {
                    if (t[1] == '+' || t[1] == '-') {
                        // 相对模式: v+50 → 150, v-30 → 70
                        volume = 100 + (int)val;
                    } else {
                        // 绝对模式: v100 → 100, v50 → 50
                        volume = (int)val;
                    }
                    if (volume < 0) volume = 0;
                } else {
                    fprintf(stderr, "Warning: line %d, invalid volume parameter '%s'\n",
                            line_num, t.c_str());
                }
            } else {
                // 尝试解析为数字（采样率）
                char *endp = nullptr;
                long val = strtol(t.c_str(), &endp, 10);
                if (endp != t.c_str() && *endp == '\0' && val > 0) {
                    rate_override = (int)val;
                } else {
                    fprintf(stderr, "Warning: line %d, ignoring unknown parameter '%s'\n",
                            line_num, t.c_str());
                }
            }
        }

        // .pcm 文件：直接加载原始 PCM/ADPCM 数据
        if (is_pcm_file(entry_filename.c_str())) {
            PdxSlot slot;
            slot.bank = current_bank;
            slot.slot_index = slot_index;
            if (!load_raw_pcm(entry_filename.c_str(), slot.data)) {
                return false;
            }
            printf("  bank %d slot %2d: raw PCM, %zu bytes (from %s)\n",
                   current_bank, slot_index, slot.data.size(), entry_filename.c_str());
            raw_slots.push_back(slot);
        } else {
            // WAV 文件：按当前 F 模式编码
            VoiceConfig cfg;
            cfg.bank          = current_bank;
            cfg.slot_index    = slot_index;
            cfg.f_mode        = current_f;
            cfg.driver        = driver;
            cfg.rate_override    = rate_override;
            cfg.stereo_override  = stereo_override;
            cfg.volume           = volume;
            cfg.wav_filename     = entry_filename;
            configs.push_back(cfg);
        }
    }

    out_driver = driver;
    return true;
}

// ============================================================================
//  使用帮助
// ============================================================================

static void print_usage(const char* prog) {
    printf("wav2pdx -- X68000 PDX compiler (PCM8A / PCM8++ compatible)\n\n");
    printf("Supported input formats: WAV, MP3, FLAC, OGG, AIFF, AAC, M4A, WMA, etc.\n");
    printf("                        (non-WAV formats auto-transcoded via ffmpeg)\n\n");
    printf("Usage:\n");
    printf("  Single file mode:\n");
    printf("    %s -o <output.pdx> -F <f_val> [-d <driver>] [-r <hz>] [-s <slot>] [-v <vol>] <input>\n\n", prog);
    printf("  Manifest mode:\n");
    printf("    %s -o <output.pdx> -m <manifest.pdl>\n\n", prog);
    printf("Options:\n");
    printf("  -o <file>    Output PDX filename\n");
    printf("  -F <n>       F mode value (single file mode, default 4)\n");
    printf("               F0-F4: ADPCM 3.9k-15.6kHz\n");
    printf("               F5-F12: PCM8A extended modes\n");
    printf("               F5-F38: PCM8++ extended modes\n");
    printf("  -d <drv>     Driver: none, pcm8a, pcm8pp (default pcm8pp)\n");
    printf("  -r <hz>      Sample rate override (resample to specified rate)\n");
    printf("  -s <0-95>    Slot number (single file mode, default 0)\n");
    printf("  -v <vol>     Volume percent (default 100, e.g. 50=half, 200=double)\n");
    printf("  -m <file>    Manifest file\n");
    printf("  -h           Show help\n\n");
    printf("Dependency: non-WAV input requires ffmpeg (https://ffmpeg.org)\n\n");
    printf("Manifest file (#ex-pcm  #mode  F<n>@<bank>  N=filename[,hz][,v<vol>]):\n");
    printf("  #ex-pcm 2        Driver: 0=ADPCM only, 1=PCM8A, 2=PCM8++\n");
    printf("  #mode 4          Global default F value\n");
    printf("  F4@0             Switch to F4 ADPCM, bank 0\n");
    printf("  F13@1            Switch to F13 PCM8++ 44.1kHz 16bit mono, bank 1\n");
    printf("  0=kick.wav       slot 0, encode with current F mode\n");
    printf("  1=bgm.mp3        slot 1, auto-transcode MP3 -> WAV\n");
    printf("  2=vocal.flac,44100       slot 2, override sample rate to 44100Hz\n");
    printf("  3=pad.wav,v50            slot 3, volume 50%% (half)\n");
    printf("  4=lead.wav,v+50          slot 4, volume +50 (=150%%)\n");
    printf("  5=fx.wav,v-30            slot 5, volume -30 (=70%%)\n");
}

// ============================================================================
//  主入口
// ============================================================================

int main(int argc, char* argv[]) {
    const char* output_file   = nullptr;
    const char* manifest_file = nullptr;
    const char* input_file    = nullptr;
    int f_mode        = 4;           // 默认 F4 = ADPCM 15625Hz
    int slot_index    = 0;
    int rate_override = 0;
    int volume        = 100;         // 音量百分比（默认100=不变）
    PcmDriver driver  = DRIVER_PCM8PP;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            manifest_file = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            slot_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            f_mode = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rate_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            volume = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "none") == 0)        driver = DRIVER_NONE;
            else if (strcmp(argv[i], "pcm8a") == 0)  driver = DRIVER_PCM8A;
            else if (strcmp(argv[i], "pcm8pp") == 0) driver = DRIVER_PCM8PP;
            else {
                fprintf(stderr, "Unknown driver: %s (options: none, pcm8a, pcm8pp)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // ── 自动检测拖入的 .pdl/.txt 文件 ─────────────────────
    // 无 -o、-m 参数，但有无前缀输入文件且扩展名为 .pdl/.txt 时
    // 自动进入清单模式，输出文件名 = 同路径同名.pdx
    static std::string auto_output;  // static 保证 output_file 指针有效
    if (!output_file && !manifest_file && input_file) {
        const char* ext = strrchr(input_file, '.');
        if (ext && (
#ifdef _WIN32
            _stricmp(ext, ".pdl") == 0 || _stricmp(ext, ".txt") == 0
#else
            strcasecmp(ext, ".pdl") == 0 || strcasecmp(ext, ".txt") == 0
#endif
        )) {
            manifest_file = input_file;
            input_file = nullptr;
            // 输出文件名：同路径同名，扩展名改为 .pdx
            auto_output = std::string(manifest_file, ext - manifest_file) + ".pdx";
            output_file = auto_output.c_str();
            printf("Auto manifest mode: %s -> %s\n", manifest_file, output_file);
        }
    }

    // 验证
    if (!output_file) {
        fprintf(stderr, "Error: Must specify output file (-o)\n\n");
        print_usage(argv[0]);
        return 1;
    }

    std::vector<VoiceConfig> configs;
    std::vector<PdxSlot>     slots;

    if (manifest_file) {
        // 清单模式
        PcmDriver manifest_driver = DRIVER_PCM8PP;
        if (!parse_manifest(manifest_file, configs, slots, manifest_driver)) {
            return 1;
        }
        if (configs.empty() && slots.empty()) {
            fprintf(stderr, "Error: Manifest file is empty\n");
            return 1;
        }
    } else if (input_file) {
        // 单文件模式
        VoiceConfig cfg;
        cfg.bank            = 0;
        cfg.slot_index      = slot_index;
        cfg.f_mode          = f_mode;
        cfg.driver          = driver;
        cfg.rate_override   = rate_override;
        cfg.stereo_override = -1;
        cfg.volume          = volume;
        cfg.wav_filename    = input_file;
        configs.push_back(cfg);
    } else {
        fprintf(stderr, "Error: Must specify input file or manifest file\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // 处理所有 voice
    printf("wav2pdx: Compiling %zu samples...\n", configs.size() + slots.size());
    for (size_t i = 0; i < configs.size(); i++) {
        PdxSlot slot;
        if (!process_voice(configs[i], slot)) {
            fprintf(stderr, "Processing '%s' failed\n", configs[i].wav_filename.c_str());
            return 1;
        }
        slots.push_back(slot);
    }

    // 写入 PDX
    if (!write_pdx(output_file, slots)) {
        return 1;
    }

    printf("Done: Written to '%s'\n", output_file);
    return 0;
}

