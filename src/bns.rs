use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

use anyhow::{Context, Result, bail};

const BNS_MAGIC: &[u8; 4] = b"BNS ";
const INFO_MAGIC: &[u8; 4] = b"INFO";
const DATA_MAGIC: &[u8; 4] = b"DATA";
const IMD5_MAGIC: &[u8; 4] = b"IMD5";
const IMD5_HEADER_SIZE: usize = 0x20;
const BNS_FORMAT_DSP_ADPCM: u8 = 0x00;

pub fn decode_bns_to_wav(bns_data: &[u8], output_path: &Path) -> Result<(u32, usize, usize)> {
    let (sample_rate, channel_count, channels) = decode_bns(bns_data)?;
    let num_samples = channels
        .iter()
        .map(Vec::len)
        .min()
        .context("decoded audio did not contain any channels")?;
    write_wav(output_path, sample_rate, &channels)?;
    Ok((sample_rate, channel_count, num_samples))
}

pub fn decode_bns(bns_data: &[u8]) -> Result<(u32, usize, Vec<Vec<i16>>)> {
    let bns_data = if bns_data.starts_with(IMD5_MAGIC) {
        bns_data
            .get(IMD5_HEADER_SIZE..)
            .context("IMD5 header is truncated")?
    } else {
        bns_data
    };

    if !bns_data.starts_with(BNS_MAGIC) {
        if bns_data.starts_with(b"RSTM") {
            bail!("file is BRSTM format, not BNS; BRSTM decoding is not supported yet");
        }
        let magic = &bns_data[..bns_data.len().min(4)];
        bail!("expected BNS magic, got {:?}", magic);
    }

    let (info, audio_data) = read_bns_chunks(bns_data)?;
    if info.len() < 0x14 {
        bail!("INFO chunk is too small to contain the required fields");
    }

    let format = read_u8(info, 0, "BNS format byte")?;
    let loop_flag = read_u8(info, 1, "BNS loop flag")?;
    let channel_count = usize::from(read_u8(info, 2, "BNS channel count")?);
    let sample_rate = u32::from(read_be_u16(info, 4, "BNS sample rate")?);
    let loop_start = usize::try_from(read_be_u32(info, 8, "BNS loop start")?)
        .context("BNS loop start does not fit in memory")?;
    let num_samples = usize::try_from(read_be_u32(info, 0x0C, "BNS sample count")?)
        .context("BNS sample count does not fit in memory")?;
    let channel_table_offset =
        usize::try_from(read_be_u32(info, 0x10, "BNS channel table offset")?)
            .context("BNS channel table offset does not fit in memory")?;

    if format != BNS_FORMAT_DSP_ADPCM {
        bail!("unsupported BNS format byte: 0x{format:02X}");
    }
    if channel_count == 0 {
        bail!("BNS file reports zero channels");
    }
    if !matches!(loop_flag, 0 | 1) {
        bail!("unsupported BNS loop flag: {loop_flag}");
    }
    if loop_start > num_samples {
        bail!("BNS loop start is beyond the reported sample count");
    }

    let mut channel_offsets = Vec::with_capacity(channel_count);
    let mut channel_coefs = Vec::with_capacity(channel_count);
    for channel in 0..channel_count {
        let chan_info_offset = usize::try_from(read_be_u32(
            info,
            channel_table_offset + channel * 4,
            "BNS channel info offset",
        )?)
        .context("BNS channel info offset does not fit in memory")?;

        let data_offset = usize::try_from(read_be_u32(
            info,
            chan_info_offset,
            "BNS channel data offset",
        )?)
        .context("BNS channel data offset does not fit in memory")?;
        let coef_offset = usize::try_from(read_be_u32(
            info,
            chan_info_offset + 4,
            "BNS coefficient table offset",
        )?)
        .context("BNS coefficient table offset does not fit in memory")?;
        let reserved = read_be_u32(info, chan_info_offset + 8, "BNS reserved channel value")?;

        if reserved != 0 {
            bail!("unexpected non-zero value in channel info for channel {channel}");
        }
        if data_offset >= audio_data.len() {
            bail!("channel {channel} data offset points past the DATA payload");
        }

        channel_offsets.push(data_offset);
        channel_coefs.push(read_coefs(info, coef_offset)?);
    }

    let mut sorted_offsets = channel_offsets.clone();
    sorted_offsets.sort_unstable();

    let mut channels = Vec::with_capacity(channel_count);
    for (channel, start) in channel_offsets.iter().copied().enumerate() {
        let end = sorted_offsets
            .iter()
            .copied()
            .find(|offset| *offset > start)
            .unwrap_or(audio_data.len());
        let channel_data = audio_data
            .get(start..end)
            .context("channel data range points outside the DATA payload")?;
        channels.push(decode_adpcm_channel(
            channel_data,
            &channel_coefs[channel],
            num_samples,
        )?);
    }

    Ok((sample_rate, channel_count, channels))
}

fn read_bns_chunks(bns_data: &[u8]) -> Result<(&[u8], &[u8])> {
    if bns_data.len() < 0x20 {
        bail!("BNS file is too small to contain a valid header");
    }
    if !bns_data.starts_with(BNS_MAGIC) {
        bail!(
            "expected BNS magic, got {:?}",
            &bns_data[..bns_data.len().min(4)]
        );
    }
    if read_be_u32(bns_data, 4, "BNS version")? != 0xFEFF_0100 {
        bail!("unsupported BNS header version or byte order");
    }

    let file_size = usize::try_from(read_be_u32(bns_data, 8, "BNS file size")?)
        .context("BNS file size does not fit in memory")?;
    let header_size = usize::from(read_be_u16(bns_data, 0x0C, "BNS header size")?);
    let chunk_count = usize::from(read_be_u16(bns_data, 0x0E, "BNS chunk count")?);

    if file_size > bns_data.len() {
        bail!(
            "BNS header reports file size {}, but buffer only contains {} bytes",
            file_size,
            bns_data.len()
        );
    }
    if header_size > bns_data.len() {
        bail!("BNS header size extends beyond the available data");
    }

    let mut info_payload = None;
    let mut data_payload = None;

    for index in 0..chunk_count {
        let entry_offset = 0x10 + index * 8;
        if entry_offset + 8 > header_size {
            bail!("BNS chunk table extends beyond the header");
        }

        let chunk_offset =
            usize::try_from(read_be_u32(bns_data, entry_offset, "BNS chunk offset")?)
                .context("BNS chunk offset does not fit in memory")?;
        let chunk_size =
            usize::try_from(read_be_u32(bns_data, entry_offset + 4, "BNS chunk size")?)
                .context("BNS chunk size does not fit in memory")?;

        if chunk_offset < header_size || chunk_offset + chunk_size > bns_data.len() {
            bail!("BNS chunk points outside the available data");
        }
        if chunk_size < 8 {
            bail!("BNS chunk is too small to contain a valid header");
        }

        let chunk_magic = slice(bns_data, chunk_offset, 4, "BNS chunk magic")?;
        let payload_start = chunk_offset + 8;
        let payload_end = chunk_offset + chunk_size;
        let payload = bns_data
            .get(payload_start..payload_end)
            .context("BNS chunk payload points outside the file")?;

        if chunk_magic == INFO_MAGIC {
            info_payload = Some(payload);
        } else if chunk_magic == DATA_MAGIC {
            data_payload = Some(payload);
        }
    }

    match (info_payload, data_payload) {
        (Some(info), Some(data)) => Ok((info, data)),
        _ => bail!("BNS file is missing an INFO or DATA chunk"),
    }
}

fn read_coefs(info: &[u8], offset: usize) -> Result<Vec<i16>> {
    let bytes = slice(info, offset, 32, "BNS coefficient table")?;
    let mut coefs = Vec::with_capacity(16);
    for chunk in bytes.chunks_exact(2) {
        coefs.push(i16::from_be_bytes([chunk[0], chunk[1]]));
    }
    Ok(coefs)
}

fn decode_adpcm_channel(data: &[u8], coefs: &[i16], num_samples: usize) -> Result<Vec<i16>> {
    let mut samples = Vec::with_capacity(num_samples);
    let mut hist1 = 0i32;
    let mut hist2 = 0i32;
    let mut pos = 0usize;

    while samples.len() < num_samples && pos + 8 <= data.len() {
        let (frame_samples, next_hist1, next_hist2) =
            decode_adpcm_frame(&data[pos..pos + 8], coefs, hist1, hist2)?;
        samples.extend(frame_samples);
        hist1 = next_hist1;
        hist2 = next_hist2;
        pos += 8;
    }

    samples.truncate(num_samples);
    Ok(samples)
}

fn decode_adpcm_frame(
    frame: &[u8],
    coefs: &[i16],
    mut hist1: i32,
    mut hist2: i32,
) -> Result<(Vec<i16>, i32, i32)> {
    let header = *frame
        .first()
        .context("ADPCM frame is missing its header byte")?;
    let predictor = usize::from((header >> 4) & 0x0F);
    let scale = u32::from(header & 0x0F);

    if predictor * 2 + 1 >= coefs.len() {
        bail!("ADPCM predictor index {predictor} is out of range");
    }

    let c1 = i32::from(coefs[predictor * 2]);
    let c2 = i32::from(coefs[predictor * 2 + 1]);
    let scale_factor = 1i32 << scale;

    let mut samples = Vec::with_capacity(14);
    for byte in &frame[1..8] {
        for nibble in [byte >> 4, byte & 0x0F] {
            let signed_nibble = sign_nibble(nibble);
            let pcm =
                ((signed_nibble * scale_factor * 2048) + c1 * hist1 + c2 * hist2 + 1024) >> 11;
            let pcm = clamp16(pcm);
            samples.push(pcm);
            hist2 = hist1;
            hist1 = i32::from(pcm);
        }
    }

    Ok((samples, hist1, hist2))
}

fn write_wav(path: &Path, sample_rate: u32, channels: &[Vec<i16>]) -> Result<()> {
    let channel_count = channels.len();
    if channel_count == 0 {
        bail!("cannot write a WAV with zero channels");
    }

    let num_samples = channels
        .iter()
        .map(Vec::len)
        .min()
        .context("cannot write a WAV without sample data")?;

    let block_align = u16::try_from(channel_count * 2).context("WAV block align overflows")?;
    let byte_rate = sample_rate
        .checked_mul(u32::from(block_align))
        .context("WAV byte rate overflows")?;
    let data_chunk_size = u32::try_from(num_samples)
        .context("WAV sample count does not fit in a RIFF header")?
        .checked_mul(
            u32::try_from(channel_count)
                .context("WAV channel count does not fit in a RIFF header")?,
        )
        .and_then(|value| value.checked_mul(2))
        .context("WAV data chunk size overflows")?;
    let riff_chunk_size = 36u32
        .checked_add(data_chunk_size)
        .context("WAV RIFF chunk size overflows")?;

    let file =
        File::create(path).with_context(|| format!("failed to create {}", path.display()))?;
    let mut writer = BufWriter::new(file);

    writer.write_all(b"RIFF")?;
    writer.write_all(&riff_chunk_size.to_le_bytes())?;
    writer.write_all(b"WAVE")?;
    writer.write_all(b"fmt ")?;
    writer.write_all(&16u32.to_le_bytes())?;
    writer.write_all(&1u16.to_le_bytes())?;
    writer.write_all(
        &u16::try_from(channel_count)
            .context("WAV channel count overflows")?
            .to_le_bytes(),
    )?;
    writer.write_all(&sample_rate.to_le_bytes())?;
    writer.write_all(&byte_rate.to_le_bytes())?;
    writer.write_all(&block_align.to_le_bytes())?;
    writer.write_all(&16u16.to_le_bytes())?;
    writer.write_all(b"data")?;
    writer.write_all(&data_chunk_size.to_le_bytes())?;

    for sample_index in 0..num_samples {
        for channel in channels {
            writer.write_all(&channel[sample_index].to_le_bytes())?;
        }
    }
    writer.flush()?;

    Ok(())
}

fn sign_nibble(nibble: u8) -> i32 {
    if nibble >= 8 {
        i32::from(nibble) - 16
    } else {
        i32::from(nibble)
    }
}

fn clamp16(value: i32) -> i16 {
    value.clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16
}

fn read_u8(data: &[u8], offset: usize, label: &str) -> Result<u8> {
    data.get(offset)
        .copied()
        .with_context(|| format!("missing {label} at byte offset 0x{offset:X}"))
}

fn read_be_u16(data: &[u8], offset: usize, label: &str) -> Result<u16> {
    let bytes = slice(data, offset, 2, label)?;
    Ok(u16::from_be_bytes(
        bytes.try_into().expect("slice length checked"),
    ))
}

fn read_be_u32(data: &[u8], offset: usize, label: &str) -> Result<u32> {
    let bytes = slice(data, offset, 4, label)?;
    Ok(u32::from_be_bytes(
        bytes.try_into().expect("slice length checked"),
    ))
}

fn slice<'a>(data: &'a [u8], offset: usize, len: usize, label: &str) -> Result<&'a [u8]> {
    let end = offset.checked_add(len).context("byte range overflows")?;
    data.get(offset..end)
        .with_context(|| format!("missing {label} at byte range 0x{offset:X}..0x{end:X}"))
}

#[cfg(test)]
mod tests {
    use super::{clamp16, sign_nibble};

    #[test]
    fn sign_nibble_matches_dsp_rules() {
        assert_eq!(sign_nibble(0x0), 0);
        assert_eq!(sign_nibble(0x7), 7);
        assert_eq!(sign_nibble(0x8), -8);
        assert_eq!(sign_nibble(0xF), -1);
    }

    #[test]
    fn clamp16_limits_pcm_range() {
        assert_eq!(clamp16(-40_000), i16::MIN);
        assert_eq!(clamp16(40_000), i16::MAX);
        assert_eq!(clamp16(1234), 1234);
    }
}
