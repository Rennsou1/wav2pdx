# wav2pdx

X68000 PDX 编译器，支持 PCM8A / PCM8++ 驱动。

将音频文件编码为 ADPCM / 16bit PCM / 8bit PCM 格式，写入 PDX 文件。

支持 WAV 直接输入，其他格式（MP3、FLAC、OGG、AIFF、AAC、M4A、WMA 等）通过 ffmpeg 自动转码。

## 命令行

### 单文件模式

```
wav2pdx -o <output.pdx> -F <f_val> [-d <driver>] [-r <hz>] [-s <slot>] <input>
```

`<input>` 可以是 `.wav` 文件或任何 ffmpeg 支持的音频格式。

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-o <file>` | 输出 PDX 文件名 | 必填 |
| `-F <n>` | F 模式值（决定编码格式、采样率、声道） | `4` |
| `-d <drv>` | 驱动：`none` / `pcm8a` / `pcm8pp` | `pcm8pp` |
| `-r <hz>` | 采样率覆盖（重采样到指定频率） | 由 F 值决定 |
| `-s <n>` | 槽位号（0–95） | `0` |

### 清单模式

```
wav2pdx -o <output.pdx> -m <manifest.pdl>
```

---

## PDL 语法

PDL 采用 MML 指令，逐行解析。

### 指令一览

#### `#ex-pcm <0|1|2>` — 驱动选择

指定目标 PCM 驱动，决定 F 值的解释方式。

| 值 | 驱动 | 说明 |
|:--:|------|------|
| 0 | 纯 ADPCM | 无扩展驱动，仅 F0–F4 |
| 1 | PCM8A | philly 1993-1997，F0–F12 |
| 2 | PCM8++ | たにぃ 1994-1996，F0–F38 |

省略时默认 `2`（PCM8++）。

#### `#mode <f_val>` — 全局默认 F 值

设置后续所有未被 `F` 指令覆盖的条目的默认 F 值。省略时默认 `4`。

#### `#ex-pdx <n>` — EX-PDX 声明

信息行，不影响编码。

#### `# <text>` — 注释

以 `#` 开头的其他行均视为注释，被忽略。

---

#### `F<n>@<bank>` — 模式/Bank 切换

独占一行，切换当前 F 模式和/或 bank。后续的采样条目使用此设置。

| 写法 | 含义 |
|------|------|
| `F4@0` | F 模式设为 4，bank 设为 0 |
| `F13@1` | F 模式设为 13，bank 设为 1 |
| `F4` | F 模式设为 4，bank 不变 |
| `@1` | bank 设为 1，F 模式不变 |

---

#### `N=filename[,hz][,stereo|mono]` — 采样条目

将 WAV 文件编码并写入指定槽位。

| 字段 | 说明 |
|------|------|
| `N` | 槽位号（0–95） |
| `filename` | WAV 或 PCM 文件名 |
| `,hz` | 可选，覆盖目标采样率（仅用于重采样） |
| `,stereo` / `,mono` | 可选，强制覆盖声道数 |

- `.wav` 文件按当前 F 模式编码
- `.pcm` 文件直接加载原始数据（不重新编码）
- 其他格式（`.mp3`、`.flac`、`.ogg` 等）通过 ffmpeg 自动转码为 WAV 后编码
- 可变频率模式（F7/F15/F23/F31/F39）必须指定 `,hz`

---

## F 值参考

详见 [F值表.md](F值表.md)。

### 快速参考

| F 值 | 通用 | PCM8A | PCM8++ | 备注 |
|:----:|------|-------|--------|------|
| 0–4 | ADPCM 3.9k–15.6kHz | 同左 | 同左 | |
| 5–6 | — | PCM16/PCM8 15.6kHz | 同左 | |
| 7 | — | ADPCM 20.8kHz | 16bit 须指定 hz | **可变频率** † |
| 8–12 | — | PCM16/PCM8/ADPCM 20.8k–31.2kHz | PCM16 15.6k–32kHz | |
| 13–14 | — | — | PCM16 44.1k/48kHz | |
| 15 | — | — | 16bit 须指定 hz | **可变频率** † |
| 16–22 | — | — | PCM8 15.6k–48kHz | |
| 23 | — | — | 8bit 须指定 hz | **可变频率** † |
| 24–30 | — | — | PCM16 Stereo 15.6k–48kHz | |
| 31 | — | — | 16bit Stereo 须指定 hz | **可变频率** † |
| 32–38 | — | — | PCM8 Stereo 15.6k–48kHz | |
| 39 | — | — | 8bit Stereo 须指定 hz | **可变频率** † |

† **可变频率模式（MXDRVp驱动支持）**：首个音符捕获为 base，后续音符自动按 `rate × 2^(Δnote/12)` 变调。必须在 PDL 中指定 `,hz`。采样率会自动嵌入 PDX 文件元数据，使 MXDRVp 能按原始采样率播放。

## PDX 采样率元数据（扩展格式）

当使用可变频率模式（F7/F15/F23/F31/F39–F41）时，wav2pdx 会在 PDX 文件末尾追加采样率元数据块，供 MXDRVp 驱动读取。

### 元数据块结构

```
[标准 PDX 数据: 头表 + 音频]
["PDXr"(4B)] [version(2B BE)] [count(2B BE)] [entries(N×6B)] ["rXDP"(4B)]
```

每个 entry 为 `slot_index(2B BE) + sample_rate(4B BE)`。

### 向后兼容性

- 旧版播放器会忽略尾部多余数据，不影响原有 PDX 解析
- 仅当存在可变频率槽时才追加，固定频率模式的 PDX 文件不受影响
- mdx2wav 通过尾部 `"rXDP"` 签名检测，找不到则回退到默认 15625Hz

## 依赖

- **ffmpeg**（仅在输入非 WAV 格式时需要，须在系统 PATH 中）
  - 下载: https://ffmpeg.org

## 构建

```bash
# MSYS2 MinGW64
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
make
```
