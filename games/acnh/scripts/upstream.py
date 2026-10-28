"""复用游戏容器提取器并执行 ACNH 标题校验。"""

from __future__ import annotations

from contextlib import contextmanager
import importlib
from pathlib import Path
import sys
from typing import Iterator

BASE_TITLE_ID = "01006F8002326000"
UPDATE_TITLE_ID = "01006F8002326800"
REPO_ROOT = Path(__file__).resolve().parents[3]
EXTRACTORS = REPO_ROOT / "engine/tools/game-data-extractors"


class AssetError(ValueError):
    """输入不符合提取工具的安全前提。"""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssetError(message)


@contextmanager
def upstream_modules(keys: dict[str, bytes] | None = None) -> Iterator[tuple[object, object, object]]:
    require(EXTRACTORS.is_dir(), "游戏数据提取工具缺失")
    scripts = str(EXTRACTORS)
    sys.path.insert(0, scripts)
    try:
        for name in ("extract_exefs", "extract_romfs", "extract_update_romfs", "extract_shared_fonts"):
            sys.modules.pop(name, None)
        exefs = importlib.import_module("extract_exefs")
        romfs = importlib.import_module("extract_romfs")
        merge = importlib.import_module("extract_update_romfs")
        original_program_nca = exefs.program_nca
        original_loaders = (exefs.load_keys, romfs.load_keys)

        def acnh_program_nca(records, update=False):
            allowed = {BASE_TITLE_ID, UPDATE_TITLE_ID}
            require(all(record["title_id"] in allowed for record, _ in records), "输入包包含其他游戏或混合标题")
            programs = [(record, header) for record, header in records if record["content_type"] == 0]
            metadata = [record for record, _ in records if record["content_type"] == 1]
            require(len(programs) == 1, "输入包必须且只能包含一个 ACNH Program NCA")
            record, header = programs[0]
            require(record["title_id"] == BASE_TITLE_ID, "Program NCA 不是 ACNH 主程序")
            if update:
                require(any(item["title_id"] == UPDATE_TITLE_ID for item in metadata), "更新包缺少 ACNH update Meta NCA")
                require(any(header[0x400 + i * 512 + 4] == 4 for i in range(4)), "更新 Program NCA 不含 BKTR")
            else:
                require(not metadata or all(item["title_id"] == BASE_TITLE_ID for item in metadata), "基础包混入更新或 DLC")
                require(not any(header[0x400 + i * 512 + 4] == 4 for i in range(4)), "基础包误包含 BKTR 更新")
            return record, header

        exefs.program_nca = acnh_program_nca
        romfs.program_nca = acnh_program_nca
        merge.program_nca = acnh_program_nca
        if keys is not None:
            exefs.load_keys = lambda _path: keys
            romfs.load_keys = lambda _path: keys
        try:
            yield exefs, romfs, merge
        finally:
            exefs.program_nca = original_program_nca
            exefs.load_keys, romfs.load_keys = original_loaders
    finally:
        sys.path.remove(scripts)
        for name in ("extract_exefs", "extract_romfs", "extract_update_romfs", "extract_shared_fonts"):
            sys.modules.pop(name, None)


@contextmanager
def upstream_font_module(keys: dict[str, bytes]) -> Iterator[object]:
    with upstream_modules(keys):
        fonts = importlib.import_module("extract_shared_fonts")
        original_loader = fonts.load_keys
        fonts.load_keys = lambda _path: keys
        try:
            yield fonts
        finally:
            fonts.load_keys = original_loader
