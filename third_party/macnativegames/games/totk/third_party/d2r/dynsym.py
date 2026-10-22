"""NSO 映像的有界符号计数与 call-window 目标校验。"""

import struct


def symbol_count(image, header, ro_off, tags):
    """从 DT_HASH 或 NSO dynsym 范围计算符号数。"""
    if 4 in tags:
        offset = tags[4]
        if offset < 0 or offset + 8 > len(image):
            raise ValueError('DT_HASH header outside image')
        _, count = struct.unpack_from('<II', image, offset)
    else:
        if len(header) < 0xA0:
            raise ValueError('NSO header is too short for dynsym extent')
        _, _, symbol_offset, symbol_size = struct.unpack_from('<IIII', header, 0x90)
        if ro_off + symbol_offset != tags.get(6):
            raise ValueError('NSO dynsym offset does not match DT_SYMTAB')
        if symbol_size == 0 or symbol_size % 24:
            raise ValueError('Invalid NSO dynsym size')
        if tags[6] < 0 or tags[6] + symbol_size > len(image):
            raise ValueError('NSO dynsym extent outside image')
        count = symbol_size // 24
    if count == 0 or tags.get(11) != 24:
        raise ValueError('Invalid ELF64 dynsym table')
    if tags.get(6, -1) < 0 or tags[6] + count * 24 > len(image):
        raise ValueError('dynsym table outside image')
    return count


def assert_call_targets_safe(image, tags, count, call_targets, decode_relr, data_off, key_page_off):
    """拒绝符号或相对指针指向 call-window 的 BLR 指令字。"""
    for where, addend in decode_relr(image, tags, data_off, key_page_off):
        if addend in call_targets:
            raise ValueError(f'RELR pointer enters unprepared call: {where:x}')
    for i in range(count):
        _, _, _, section, value, _ = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        if section and value in call_targets:
            raise ValueError('dynamic symbol enters unprepared call')
    for table, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
        if table:
            if size % 24 or table < 0 or table + size > len(image):
                raise ValueError('Invalid RELA table extent')
            for off in range(table, table + size, 24):
                _, info, addend = struct.unpack_from('<QQq', image, off)
                if info & 0xFFFFFFFF == 1027 and addend in call_targets:
                    raise ValueError('relative pointer enters unprepared call')
