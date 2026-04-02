"""
Decoder for Nintendo BNS (Binary Sound) audio format → PCM WAV.

BNS file layout:
  0x00  magic       "BNS " (4 bytes)
  0x04  byte_order  0xFEFF = big-endian
  0x08  file_size   total file size
  0x0C  header_size usually 0x20
  0x0E  chunk_count usually 2 (INFO + DATA)
  0x10  chunk table entries: (offset, size) pairs relative to BNS start

INFO block (chunk payload, after the 8-byte INFO chunk header):
  0x00  format byte  usually 0x00 for Nintendo DSP-ADPCM
  0x01  loop_flag
  0x02  channel_count
  0x03  zero
  0x04  sample_rate  (u16)
  0x06  zero
  0x08  loop_start   (u32, in samples)
  0x0C  sample_count (u32)
  0x10  channel_info_table offset (relative to INFO payload)
  ...

Channel info (pointed to by channel_info_table):
  Each entry is an offset (u32) relative to INFO payload → ChannelInfo struct
  ChannelInfo:
    0x00  data_offset  offset to the channel's ADPCM data (relative to DATA payload)
    0x04  coef_offset  offset to ADPCM coefficient table (relative to INFO payload)
    0x08  zero

ADPCM coefficient table (16 x s16 = 32 bytes):
  These are the 8 pairs of predictor coefficients used by DSP-ADPCM

DATA block:
  0x00  magic   "DATA"
  0x04  size
  0x08  audio data (commonly stored as one contiguous range per channel)

DSP-ADPCM decoding:
  Each block of 8 bytes decodes to 14 PCM samples.
  Byte 0: header — upper nibble = predictor index (0-7), lower nibble = scale shift
  Bytes 1-7: 7 pairs of nibbles = 14 signed 4-bit samples
  Decode: sample = ((nibble << scale) + coef1*hist1 + coef2*hist2 + 1024) >> 11
  Clamp to [-32768, 32767]
"""

import struct
import wave
from pathlib import Path


BNS_MAGIC = b"BNS "
INFO_MAGIC = b"INFO"
DATA_MAGIC = b"DATA"
IMD5_MAGIC = b"IMD5"
IMD5_HEADER_SIZE = 0x20  # 32 bytes: magic(4) + content_size(4) + padding(8) + md5(16)

# Wii banner BNS uses Nintendo DSP-ADPCM when this format byte is 0.
BNS_FORMAT_DSP_ADPCM = 0x00


def _sign_nibble(n: int) -> int:
    """Convert unsigned 4-bit nibble to signed."""
    return n - 16 if n >= 8 else n


def _clamp16(v: int) -> int:
    return max(-32768, min(32767, v))


def _decode_adpcm_frame(
    frame: bytes, coefs: list[int], hist1: int, hist2: int
) -> tuple[list[int], int, int]:
    """
    Decode one DSP-ADPCM frame (8 bytes → 14 samples).
    Returns (samples, new_hist1, new_hist2).
    coefs: list of 16 s16 values (8 pairs)
    """
    header = frame[0]
    predictor = (header >> 4) & 0x0F
    scale = header & 0x0F

    c1 = coefs[predictor * 2]
    c2 = coefs[predictor * 2 + 1]
    scale_factor = 1 << scale

    samples: list[int] = []
    for byte in frame[1:8]:
        for nibble in (byte >> 4, byte & 0x0F):
            s = _sign_nibble(nibble)
            pcm = (s * scale_factor * 2048 + c1 * hist1 + c2 * hist2 + 1024) >> 11
            pcm = _clamp16(pcm)
            samples.append(pcm)
            hist2 = hist1
            hist1 = pcm

    return samples, hist1, hist2


def _decode_adpcm_channel(data: bytes, coefs: list[int], num_samples: int) -> list[int]:
    """Decode all frames for a single channel."""
    samples: list[int] = []
    hist1 = 0
    hist2 = 0
    pos = 0
    while len(samples) < num_samples and pos + 8 <= len(data):
        frame_samples, hist1, hist2 = _decode_adpcm_frame(
            data[pos : pos + 8], coefs, hist1, hist2
        )
        samples.extend(frame_samples)
        pos += 8
    return samples[:num_samples]


def _read_bns_chunks(bns_data: bytes) -> tuple[bytes, bytes]:
    """Return the INFO and DATA chunk payloads from a BNS file."""
    if len(bns_data) < 0x20:
        raise ValueError("BNS file is too small to contain a valid header")

    if bns_data[:4] != BNS_MAGIC:
        raise ValueError(f"Expected BNS magic, got {bns_data[:4]!r}")

    if struct.unpack_from(">I", bns_data, 4)[0] != 0xFEFF0100:
        raise ValueError("Unsupported BNS header version or byte order")

    file_size = struct.unpack_from(">I", bns_data, 8)[0]
    header_size, chunk_count = struct.unpack_from(">HH", bns_data, 0x0C)

    if file_size > len(bns_data):
        raise ValueError(
            f"BNS header reports file size {file_size:,}, but buffer only contains {len(bns_data):,} bytes"
        )
    if header_size > len(bns_data):
        raise ValueError("BNS header size extends beyond the available data")

    info_payload = None
    data_payload = None

    for index in range(chunk_count):
        entry_offset = 0x10 + index * 8
        if entry_offset + 8 > header_size:
            raise ValueError("BNS chunk table extends beyond the header")

        chunk_offset_rel, chunk_size = struct.unpack_from(">II", bns_data, entry_offset)
        chunk_offset = chunk_offset_rel

        if chunk_offset < header_size or chunk_offset + chunk_size > len(bns_data):
            raise ValueError("BNS chunk points outside the available data")
        if chunk_size < 8:
            raise ValueError("BNS chunk is too small to contain a valid header")

        chunk_magic = bns_data[chunk_offset : chunk_offset + 4]
        payload_start = chunk_offset + 8
        payload_end = chunk_offset + chunk_size
        payload = bns_data[payload_start:payload_end]

        if chunk_magic == INFO_MAGIC:
            info_payload = payload
        elif chunk_magic == DATA_MAGIC:
            data_payload = payload

    if info_payload is None or data_payload is None:
        raise ValueError("BNS file is missing an INFO or DATA chunk")

    return info_payload, data_payload


def decode_bns(bns_data: bytes) -> tuple[int, int, list[list[int]]]:
    """
    Decode BNS audio data.

    Returns:
        (sample_rate, channel_count, channels)
        where channels is a list of lists of s16 PCM samples, one per channel.
    """
    # Strip IMD5 wrapper if present (Nintendo hash header)
    if bns_data[:4] == IMD5_MAGIC:
        bns_data = bns_data[IMD5_HEADER_SIZE:]

    if bns_data[:4] != BNS_MAGIC:
        # Check if it might be a BRSTM or other format
        if bns_data[:4] == b"RSTM":
            raise ValueError(
                "File is BRSTM format, not BNS — BRSTM decoding not yet supported"
            )
        raise ValueError(f"Expected BNS magic, got {bns_data[:4]!r}")

    try:
        info, audio_data = _read_bns_chunks(bns_data)

        if len(info) < 0x14:
            raise ValueError("INFO chunk is too small to contain the required fields")

        fmt, loop_flag, channel_count = struct.unpack_from(">BBB", info, 0)
        sample_rate = struct.unpack_from(">H", info, 4)[0]
        loop_start = struct.unpack_from(">I", info, 8)[0]
        num_samples = struct.unpack_from(">I", info, 0x0C)[0]
        channel_table_offset = struct.unpack_from(">I", info, 0x10)[0]

        if fmt != BNS_FORMAT_DSP_ADPCM:
            raise ValueError(f"Unsupported BNS format byte: 0x{fmt:02X}")
        if channel_count == 0:
            raise ValueError("BNS file reports zero channels")
        if loop_flag not in (0, 1):
            raise ValueError(f"Unsupported BNS loop flag: {loop_flag}")
        if loop_start > num_samples:
            raise ValueError("BNS loop start is beyond the reported sample count")

        channel_offsets: list[int] = []
        channel_coefs: list[list[int]] = []
        for ch in range(channel_count):
            chan_info_offset = struct.unpack_from(
                ">I", info, channel_table_offset + ch * 4
            )[0]
            data_offset, coef_offset, reserved = struct.unpack_from(
                ">III", info, chan_info_offset
            )
            if reserved != 0:
                raise ValueError(
                    f"Unexpected non-zero value in channel info for channel {ch}"
                )
            if data_offset >= len(audio_data):
                raise ValueError(
                    f"Channel {ch} data offset points past the DATA payload"
                )

            channel_offsets.append(data_offset)
            channel_coefs.append(list(struct.unpack_from(">16h", info, coef_offset)))
    except struct.error as exc:
        raise ValueError("Malformed BNS structure") from exc

    channels = []
    sorted_offsets = sorted(channel_offsets)
    for ch, start in enumerate(channel_offsets):
        next_offsets = [offset for offset in sorted_offsets if offset > start]
        end = next_offsets[0] if next_offsets else len(audio_data)
        channels.append(
            _decode_adpcm_channel(audio_data[start:end], channel_coefs[ch], num_samples)
        )

    return sample_rate, channel_count, channels


def write_wav(path: Path, sample_rate: int, channels: list[list[int]]) -> None:
    """Write PCM samples to a WAV file."""
    channel_count = len(channels)
    num_samples = min(len(ch) for ch in channels)

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channel_count)
        wav.setsampwidth(2)  # 16-bit
        wav.setframerate(sample_rate)

        # Interleave channels
        frames = bytearray()
        for i in range(num_samples):
            for ch in range(channel_count):
                frames.extend(struct.pack("<h", channels[ch][i]))
        wav.writeframes(bytes(frames))


def decode_bns_to_wav(bns_data: bytes, output_path: Path) -> tuple[int, int, int]:
    """
    Decode BNS audio and write to a WAV file.

    Returns (sample_rate, channel_count, num_samples).
    """
    sample_rate, channel_count, channels = decode_bns(bns_data)
    num_samples = min(len(ch) for ch in channels)
    write_wav(output_path, sample_rate, channels)
    return sample_rate, channel_count, num_samples
