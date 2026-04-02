"""
Parser for Wii opening.bnr files.

opening.bnr structure:
  - IMET header (0x600 bytes) — contains game title strings, icon/banner hash
  - U8 archive — Nintendo container holding banner.bin, icon.bin, sound.bin

U8 archive format (magic 0x55AA382D):
  - Header: magic, root_node_offset, header_size+nodes_size, data_offset
  - Root node (directory): type=0x01, name_offset, parent_index, child_count
  - File nodes: type=0x00, name_offset, data_offset, data_size
  - String table immediately follows the node table
  - File data starts at data_offset
"""

import struct
from dataclasses import dataclass


IMET_MAGIC = b"IMET"
U8_MAGIC = b"\x55\xaa\x38\x2d"


@dataclass
class U8Node:
    is_dir: bool
    name_offset: int
    data_offset_or_parent: int  # data offset for files, parent index for dirs
    size_or_next_dir: int  # size for files, next_dir index for dirs
    name: str = ""


def _find_imet_offset(data: bytes) -> int:
    """Find the offset of the IMET magic in the data."""
    offset = data.find(IMET_MAGIC)
    if offset == -1:
        raise ValueError("IMET magic not found in opening.bnr")
    return offset


def _find_u8_offset(data: bytes, start: int = 0) -> int:
    """Find the offset of the U8 archive magic after the IMET header."""
    offset = data.find(U8_MAGIC, start)
    if offset == -1:
        raise ValueError("U8 archive magic not found in opening.bnr")
    return offset


def _parse_u8_nodes(data: bytes) -> list[U8Node]:
    """
    Parse the U8 node table and string table.
    data should start at the beginning of the U8 archive.

    Each U8 node is 12 bytes:
      - 4 bytes big-endian: upper byte = type (0=file, 1=dir), lower 3 bytes = name offset
      - 4 bytes: data offset (file) or parent node index (dir)
      - 4 bytes: data size (file) or last-child node index (dir)
    """
    # U8 header: magic(4), root_node_offset(4), header_and_nodes_size(4), data_offset(4), reserved(16)
    magic, root_node_offset, header_nodes_size, data_offset = struct.unpack_from(
        ">4sIII", data, 0
    )
    if magic != U8_MAGIC:
        raise ValueError(f"Expected U8 magic, got {magic!r}")

    node_fmt = ">III"  # type+name(4), field2(4), field3(4) — 12 bytes per node
    node_size = struct.calcsize(node_fmt)

    # Root node gives us total node count in field3
    type_and_name, root_parent, total_nodes = struct.unpack_from(
        node_fmt, data, root_node_offset
    )

    # String table immediately follows all nodes
    string_table_offset = root_node_offset + total_nodes * node_size

    def read_string(name_off: int) -> str:
        end = data.index(b"\x00", string_table_offset + name_off)
        return data[string_table_offset + name_off : end].decode(
            "ascii", errors="replace"
        )

    nodes: list[U8Node] = []
    for i in range(total_nodes):
        offset = root_node_offset + i * node_size
        type_and_name, field2, field3 = struct.unpack_from(node_fmt, data, offset)
        is_dir = bool((type_and_name >> 24) & 0xFF)
        name_off = type_and_name & 0x00FFFFFF
        node = U8Node(
            is_dir=is_dir,
            name_offset=name_off,
            data_offset_or_parent=field2,
            size_or_next_dir=field3,
        )
        node.name = read_string(name_off) if i > 0 else ""
        nodes.append(node)

    return nodes


def extract_sound_bin(bnr_data: bytes) -> bytes:
    """
    Extract sound.bin from an opening.bnr file's raw bytes.

    Returns the raw bytes of sound.bin (BNS or BRSTM audio data).
    Raises ValueError if the file cannot be parsed or sound.bin is not found.
    """
    # Find U8 archive (skip IMET header)
    imet_offset = _find_imet_offset(bnr_data)
    # U8 archive starts shortly after IMET; search from the IMET header boundary
    # IMET header is 0x600 bytes from the start of the file (the IMET magic is at 0x40 or 0x80
    # within that block), so just search for U8 magic after the IMET magic
    u8_offset = _find_u8_offset(bnr_data, imet_offset + 4)
    u8_data = bnr_data[u8_offset:]

    # U8 header gives us data_offset
    _, root_node_offset, _, data_offset = struct.unpack_from(">4sIII", u8_data, 0)

    nodes = _parse_u8_nodes(u8_data)

    for node in nodes:
        if not node.is_dir and node.name.lower() == "sound.bin":
            file_start = node.data_offset_or_parent
            file_size = node.size_or_next_dir
            return u8_data[file_start : file_start + file_size]

    raise ValueError("sound.bin not found in U8 archive inside opening.bnr")
