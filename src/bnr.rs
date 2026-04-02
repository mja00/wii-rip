use anyhow::{Context, Result, bail};

const IMET_MAGIC: &[u8; 4] = b"IMET";
const U8_MAGIC: &[u8; 4] = b"\x55\xaa\x38\x2d";
const U8_NODE_SIZE: usize = 12;

#[derive(Debug)]
struct U8Node {
    is_dir: bool,
    data_offset_or_parent: u32,
    size_or_next_dir: u32,
    name: String,
}

pub fn extract_sound_bin(bnr_data: &[u8]) -> Result<Vec<u8>> {
    let imet_offset = find_magic(
        bnr_data,
        IMET_MAGIC,
        0,
        "IMET magic not found in opening.bnr",
    )?;
    let u8_offset = find_magic(
        bnr_data,
        U8_MAGIC,
        imet_offset + IMET_MAGIC.len(),
        "U8 archive magic not found in opening.bnr",
    )?;
    let u8_data = &bnr_data[u8_offset..];

    let nodes = parse_u8_nodes(u8_data)?;

    for node in nodes {
        if !node.is_dir && node.name.eq_ignore_ascii_case("sound.bin") {
            let file_start = usize::try_from(node.data_offset_or_parent)
                .context("sound.bin offset does not fit in memory")?;
            let file_size = usize::try_from(node.size_or_next_dir)
                .context("sound.bin size does not fit in memory")?;
            let file_end = file_start
                .checked_add(file_size)
                .context("sound.bin range overflows")?;
            let sound_bin = u8_data
                .get(file_start..file_end)
                .context("sound.bin points outside the U8 archive")?;
            return Ok(sound_bin.to_vec());
        }
    }

    bail!("sound.bin not found in U8 archive inside opening.bnr")
}

fn parse_u8_nodes(data: &[u8]) -> Result<Vec<U8Node>> {
    let magic = slice(data, 0, 4, "U8 header magic")?;
    if magic != U8_MAGIC {
        bail!("expected U8 magic, got {:?}", magic);
    }

    let root_node_offset = usize::try_from(read_be_u32(data, 4, "U8 root node offset")?)
        .context("U8 root node offset does not fit in memory")?;

    let (_, total_nodes) = read_u8_node_header(data, root_node_offset)?;
    let total_nodes =
        usize::try_from(total_nodes).context("U8 node count does not fit in memory")?;

    let node_table_size = total_nodes
        .checked_mul(U8_NODE_SIZE)
        .context("U8 node table size overflows")?;
    let string_table_offset = root_node_offset
        .checked_add(node_table_size)
        .context("U8 string table offset overflows")?;

    let mut nodes = Vec::with_capacity(total_nodes);
    for index in 0..total_nodes {
        let offset = root_node_offset
            .checked_add(
                index
                    .checked_mul(U8_NODE_SIZE)
                    .context("U8 node offset overflows")?,
            )
            .context("U8 node offset overflows")?;
        let type_and_name = read_be_u32(data, offset, "U8 node type/name")?;
        let field2 = read_be_u32(data, offset + 4, "U8 node field2")?;
        let field3 = read_be_u32(data, offset + 8, "U8 node field3")?;
        let is_dir = ((type_and_name >> 24) & 0xFF) != 0;
        let name_offset = usize::try_from(type_and_name & 0x00FF_FFFF)
            .context("U8 name offset does not fit in memory")?;
        let name = if index == 0 {
            String::new()
        } else {
            read_u8_string(data, string_table_offset, name_offset)?
        };

        nodes.push(U8Node {
            is_dir,
            data_offset_or_parent: field2,
            size_or_next_dir: field3,
            name,
        });
    }

    Ok(nodes)
}

fn read_u8_node_header(data: &[u8], offset: usize) -> Result<(u32, u32)> {
    let type_and_name = read_be_u32(data, offset, "U8 root node type/name")?;
    let total_nodes = read_be_u32(data, offset + 8, "U8 root node child count")?;
    Ok((type_and_name, total_nodes))
}

fn read_u8_string(data: &[u8], string_table_offset: usize, name_offset: usize) -> Result<String> {
    let start = string_table_offset
        .checked_add(name_offset)
        .context("U8 string offset overflows")?;
    let rest = data
        .get(start..)
        .context("U8 string offset points outside the archive")?;
    let end = rest
        .iter()
        .position(|byte| *byte == 0)
        .context("U8 string is missing a null terminator")?;
    Ok(String::from_utf8_lossy(&rest[..end]).into_owned())
}

fn find_magic(data: &[u8], magic: &[u8], start: usize, error_message: &str) -> Result<usize> {
    if start > data.len() {
        bail!("{}", error_message);
    }

    data[start..]
        .windows(magic.len())
        .position(|window| window == magic)
        .map(|offset| start + offset)
        .with_context(|| error_message.to_string())
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
