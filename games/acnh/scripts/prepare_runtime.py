#!/usr/bin/env python3
"""检查 ACNH 3.0.0 模块，并为 macOS 装载器生成校验后的表。"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / "engine"
sys.path.insert(0, str(ENGINE / "tools/nso-rewriter"))
from relr import decode_relr  # noqa: E402

PROFILE = ROOT / "games/acnh/profiles/3.0.0.json"
DEFAULT_MANIFEST = ROOT / "work/acnh/3.0.0/rewritten/manifest.json"
MODULES = ("main", "subsdk0", "sdk")
REWRITE_TOOLS = {
    "games/acnh/scripts/rewrite.py",
    "games/acnh/profiles/3.0.0.json",
    "engine/tools/nso-rewriter/patch_nso.py",
    "engine/tools/nso-rewriter/dynsym.py",
    "engine/tools/nso-rewriter/call_window.py",
    "engine/tools/nso-rewriter/relr.py",
}
DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 4, 5, 6, 7, 8
DT_RELAENT, DT_INIT, DT_PLTRELSZ, DT_JMPREL, DT_PLTREL = 9, 12, 2, 23, 20
DT_SYMENT, DT_GNU_HASH, DT_INIT_ARRAY, DT_INIT_ARRAYSZ = 11, 0x6FFFFEF5, 25, 27


class RuntimeErrorDetail(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeErrorDetail(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_profile() -> dict:
    return json.loads(PROFILE.read_text())


def artifact_entry(module: dict, filename: str) -> dict:
    entries = module.get("artifacts", {})
    require(filename in entries, f"manifest is missing artifact metadata for {filename}")
    return entries[filename]


def verify_artifact(path: Path, metadata: dict, label: str) -> bytes:
    data = path.read_bytes()
    require(metadata.get("size") == len(data), f"{label}: manifest size mismatch")
    require(metadata.get("sha256") == sha256(data), f"{label}: manifest SHA-256 mismatch")
    return data


def read_fixups(path: Path) -> dict[str, int]:
    result = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        require(len(fields) == 3, f"{path}:{number}: expected three fixup columns")
        _, key, value = fields
        require(key not in result, f"{path}:{number}: duplicate fixup {key}")
        result[key] = int(value, 0)
    required = {"TEXT_END", "RO_OFF", "DATA_OFF", "PATCH_OFF", "KEY_PAGE_OFF"}
    require(required <= result.keys(), f"{path}: missing fixups {sorted(required - result.keys())}")
    return result


def read_dynamic(image: bytes, layout: dict[str, int]) -> dict[int, int]:
    mod0 = struct.unpack_from("<I", image, 4)[0]
    require(image[mod0:mod0 + 4] == b"MOD0", "missing MOD0 header")
    dyn = mod0 + struct.unpack_from("<i", image, mod0 + 4)[0]
    tags = {}
    for offset in range(dyn, min(dyn + 4096 * 16, len(image) - 15), 16):
        tag, value = struct.unpack_from("<qQ", image, offset)
        if tag == 0:
            break
        tags[tag] = value
    else:
        raise RuntimeErrorDetail("unterminated dynamic table")
    require(DT_STRTAB in tags and DT_SYMTAB in tags, "module lacks dynamic string/symbol table")
    require(tags.get(DT_SYMENT) == 24, "unsupported dynamic symbol entry size")
    require(tags.get(DT_RELAENT) == 24, "unsupported RELA entry size")
    require(tags.get(DT_PLTREL) == 7, "unsupported PLT relocation encoding")
    return tags


def symbol_count(image: bytes, tags: dict[int, int], layout: dict[str, int], raw_nso: Path) -> int:
    if DT_HASH in tags:
        return struct.unpack_from("<I", image, tags[DT_HASH] + 4)[0]
    require(DT_GNU_HASH in tags, "module has neither SYSV nor GNU hash table")
    header = raw_nso.read_bytes()[:256]
    require(len(header) >= 0xA0, f"short NSO header: {raw_nso}")
    _, _, symbol_offset, symbol_size = struct.unpack_from("<IIII", header, 0x90)
    require(layout["RO_OFF"] + symbol_offset == tags[DT_SYMTAB], "GNU-only dynsym offset disagrees with NSO header")
    require(symbol_size % 24 == 0, "GNU-only dynsym size is not a whole number of entries")
    require(0 < symbol_size <= layout["DATA_OFF"] - tags[DT_SYMTAB], "GNU-only dynsym extent outside read-only segment")
    return symbol_size // 24


def decode_symbols(image: bytes, tags: dict[int, int], count: int) -> list[dict]:
    result = []
    for index in range(count):
        no, info, other, section, value, size = struct.unpack_from("<IBBHQQ", image, tags[DT_SYMTAB] + index * 24)
        start = tags[DT_STRTAB] + no
        str_end = tags[DT_STRTAB] + tags[10] if 10 in tags else len(image)
        end = image.find(b"\0", start, str_end)
        require(end >= start, f"invalid dynsym name offset {no}")
        result.append({"name": image[start:end].decode("utf-8", "replace"), "value": value, "size": size,
                       "defined": bool(section), "binding": info >> 4, "type": info & 15})
    return result


def ctor_flag(image: bytes, init: int, layout: dict[str, int]) -> tuple[int, dict]:
    """仅识别已确认的两种构造函数状态读取指令形式。"""
    candidates = []
    for flag_reg, load_offset in ((19, 20), (20, 24)):
        if init + load_offset + 4 > len(image):
            continue
        adrp = struct.unpack_from("<I", image, init + 16)[0]
        load = struct.unpack_from("<I", image, init + load_offset)[0]
        if adrp & 0x9F00001F != (0x90000000 | flag_reg):
            continue
        if load & 0xFFC003FF != (0xB9400008 | (flag_reg << 5)):
            continue
        imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        address = ((init + 16) & ~4095) + (imm << 12) + ((load >> 10) & 0xFFF) * 4
        if layout["DATA_OFF"] <= address < layout["KEY_PAGE_OFF"]:
            candidates.append((address, {"flag_register": flag_reg, "adrp_offset": init + 16,
                                         "load_offset": init + load_offset, "state_address": address}))
    require(len(candidates) == 1, f"unrecognized or ambiguous DT_INIT constructor state-load shape at {init:#x}")
    return candidates[0]


def decode_instructions(image: bytes, address: int, count: int) -> list[dict]:
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    return [{"offset": inst.address, "mnemonic": inst.mnemonic, "op_str": inst.op_str,
             "word": struct.unpack_from("<I", image, inst.address)[0]}
            for inst in md.disasm(image[address:address + count * 4], address)]


def verify_startup_abi(main: dict, sdk: dict, out_dir: Path) -> dict:
    require(len(main.get("entry_candidates", [])) == 1, "main: nnMain export is missing or ambiguous")
    entry_symbol = main["entry_candidates"][0]
    require(entry_symbol["name"] == "nnMain" and entry_symbol["type"] == 2,
            "main: entry export is not a function named nnMain")
    require(len(sdk.get("start_candidates", [])) == 1, "sdk: expected one exported nn::init::Start overload")
    start_name = "_ZN2nn4init5StartEmmPFvvES2_S2_"
    start_symbol = sdk["start_candidates"][0]
    require(start_symbol["name"] == start_name and start_symbol["type"] == 2,
            "sdk: Start export does not match the five-argument callback ABI")
    main_image = (out_dir / "main.patched.bin").read_bytes()
    sdk_image = (out_dir / "sdk.patched.bin").read_bytes()
    entry_insns = decode_instructions(main_image, entry_symbol["value"], 9)
    require(len(entry_insns) == 9, "nnMain: truncated entry instruction sequence")
    require([(i["mnemonic"], i["op_str"]) for i in entry_insns[:3]] == [
        ("mrs", "x8, fpcr"), ("orr", "x8, x8, #0x1000000"), ("msr", "fpcr, x8")],
        "nnMain: unsupported entry prologue")
    branch = entry_insns[8]
    require(branch["mnemonic"] == "b", "nnMain: expected an unconditional branch to its initialized entry body")
    branch_target = int(branch["op_str"].lstrip("#"), 16)
    require(0 <= branch_target < main["layout"]["TEXT_END"], "nnMain branch target lies outside main text")
    start_insns = decode_instructions(sdk_image, start_symbol["value"], start_symbol["size"] // 4)
    got = [(i["mnemonic"], i["op_str"]) for i in start_insns]
    wanted = [("mov", "x19, x4"), ("mov", "x20, x3"), ("mov", "x21, x2"),
              ("mov", "x22, x1"), ("mov", "x23, x0"), ("blr", "x21"), ("blr", "x20"), ("blr", "x19")]
    require(all(item in got for item in wanted), "nn::init::Start callback ABI instructions are missing")
    positions = [got.index(item) for item in wanted]
    require(positions == sorted(positions), "nn::init::Start callback order is unsupported")
    return {"nnMain_offset": entry_symbol["value"], "nnMain_instructions": entry_insns,
            "nnMain_branch_target": branch_target, "nn_init_start_offset": start_symbol["value"],
            "nn_init_start_name": start_name, "nn_init_start_instructions": start_insns,
            "callback_abi": ["handle", "argument", "rtld_callback", "module_initializers", "module_finalizers"]}


def inspect(manifest_path: Path) -> list[dict]:
    profile = read_profile()
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get("status") == "pass", "rewrite manifest status is not pass")
    require(manifest.get("title_id") == profile["title_id"] and manifest.get("display_version") == "3.0.0",
            "rewrite manifest is for a different title or game version")
    require(manifest.get("vendor", {}).get("commit") == profile["vendor"]["commit"],
            "rewrite manifest is not bound to the pinned MacNativeGames checkout")
    romfs = ROOT / "work/acnh/3.0.0/romfs"
    romfs_index = json.loads((romfs / "index.json").read_text())
    require(romfs_index.get("status") == "pass" and romfs_index.get("romfs_sha256") == profile["romfs"]["sha256"],
            "merged ACNH RomFS index does not match the pinned profile")
    require(len(romfs_index.get("files", [])) == profile["romfs"]["files"], "merged ACNH RomFS file count mismatch")
    require(sha256((romfs / "romfs.bin").read_bytes()) == profile["romfs"]["sha256"],
            "merged ACNH RomFS image hash mismatch")
    tool_hashes = manifest.get("tools", {})
    require(set(tool_hashes) == REWRITE_TOOLS, "rewrite manifest tool set differs from the approved ACNH/TOTK inputs")
    for relative, expected in tool_hashes.items():
        tool = ROOT / relative
        require(tool.is_file() and sha256(tool.read_bytes()) == expected, f"rewrite tool hash mismatch: {relative}")
    require(set(manifest.get("modules", {})) == set(MODULES), "manifest must contain exactly main, subsdk0, and sdk")
    out_dir = manifest_path.parent
    rows = []
    for name in MODULES:
        module = manifest["modules"][name]
        patched_name, fixups_name, gates_name = f"{name}.patched.bin", f"{name}.fixups.tsv", f"{name}.gates.tsv"
        image = verify_artifact(out_dir / patched_name, artifact_entry(module, patched_name), f"{name} patched image")
        fixups_data = verify_artifact(out_dir / fixups_name, artifact_entry(module, fixups_name), f"{name} fixups")
        gates_data = verify_artifact(out_dir / gates_name, artifact_entry(module, gates_name), f"{name} gates")
        profile_module = profile["modules"][name]
        raw_path = ROOT / profile["source_exefs"] / name
        require(sha256(raw_path.read_bytes()) == profile_module["sha256"], f"{name}: source NSO disagrees with ACNH profile")
        require(module.get("source_sha256") == profile_module["sha256"], f"{name}: rewrite manifest source hash disagrees with profile")
        layout = read_fixups(out_dir / fixups_name)
        tags = read_dynamic(image, layout)
        count = symbol_count(image, tags, layout, raw_path)
        symbols = decode_symbols(image, tags, count)
        relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
                  for where, addend in decode_relr(image, tags, layout["DATA_OFF"], layout["KEY_PAGE_OFF"])]
        relocated = {reloc["where"] for reloc in relocs}
        for address, size in ((tags.get(DT_RELA, 0), tags.get(DT_RELASZ, 0)),
                              (tags.get(DT_JMPREL, 0), tags.get(DT_PLTRELSZ, 0))):
            require(size % 24 == 0, f"{name}: malformed RELA extent")
            for offset in range(address, address + size, 24):
                where, info, addend = struct.unpack_from("<QQq", image, offset)
                kind, sym = info & 0xFFFFFFFF, info >> 32
                require(kind in (257, 1025, 1026, 1027) and sym < count, f"{name}: unsupported RELA kind/symbol")
                require(layout["DATA_OFF"] <= where <= layout["KEY_PAGE_OFF"] - 8, f"{name}: relocation outside writable data")
                require(where not in relocated, f"{name}: duplicate RELR/RELA relocation")
                relocated.add(where)
                relocs.append(dict(where=where, kind=kind, symbol=sym, addend=addend))
        init = tags.get(DT_INIT, 0)
        require(init and init < layout["TEXT_END"], f"{name}: DT_INIT missing or outside text")
        flag, ctor_evidence = ctor_flag(image, init, layout)
        rows.append({"name": name, "sha256": sha256(image), "size": len(image), "raw_sha256": profile_module["sha256"],
                     "raw_path": str(raw_path.relative_to(ROOT)), "layout": layout, "tags": tags,
                     "symbols": symbols, "relocs": relocs, "gates_text": gates_data.decode("utf-8"),
                     "ctor_flag": flag, "ctor_evidence": ctor_evidence,
                     "init_bytes": image[init:init + 64].hex()})
    main = rows[0]
    main_syms = [s for s in main["symbols"] if s["defined"] and s["name"] == "nnMain"]
    sdk = rows[2]
    starts = [s for s in sdk["symbols"] if s["defined"] and s["name"].startswith("_ZN2nn4init5Start")]
    rows[0]["entry_candidates"] = main_syms
    rows[2]["start_candidates"] = starts
    target = "_ZN2nn2os13GetSystemTickEv"
    slots = [reloc["where"] for reloc in main["relocs"]
             if reloc["kind"] == 1026 and main["symbols"][reloc["symbol"]]["name"] == target]
    require(len(slots) == 1, f"main: expected one relocated {target} slot, found {len(slots)}")
    image = (out_dir / "main.patched.bin").read_bytes()
    plt = []
    lower = max(0, main["layout"]["TEXT_END"] - 0x10000)
    for address in range(lower, main["layout"]["TEXT_END"] - 12, 4):
        a, b, _, d = struct.unpack_from("<IIII", image, address)
        if a & 0x9F00001F != 0x90000010 or b & 0xFFC003FF != 0xF9400211 or d != 0xD61F0220:
            continue
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        referenced = (address & ~4095) + (imm << 12) + ((b >> 10) & 0xFFF) * 8
        if referenced == slots[0]:
            plt.append(address)
    require(len(plt) == 1, f"main: expected one verified GetSystemTick PLT, found {len(plt)}")
    main["probe_plt"], main["probe_slot"] = plt[0], slots[0]
    main["startup_evidence"] = verify_startup_abi(main, rows[2], out_dir)
    return rows


def disassemble_evidence(rows: list[dict], manifest_path: Path) -> dict:
    manifest = json.loads(manifest_path.read_text())
    out_dir = manifest_path.parent
    result = {"game": "Animal Crossing: New Horizons", "version": "3.0.0", "status": "verified-entry-and-ctor-shapes",
              "source_manifest": str(manifest_path.relative_to(ROOT)), "modules": []}
    for row in rows:
        module_data = (out_dir / f"{row['name']}.patched.bin").read_bytes()
        init = row["tags"].get(DT_INIT, 0)
        insns = decode_instructions(module_data, init, 16)
        item = {"name": row["name"], "sha256": row["sha256"], "dt_init": init,
                "dt_init_instructions": insns, "ctor_state": row["ctor_evidence"]}
        if row["name"] == "main":
            item["entry_candidates"] = row.get("entry_candidates", [])
        if row["name"] == "sdk":
            item["nn_init_start_exports"] = row.get("start_candidates", [])
            item["startup_abi"] = row.get("startup_evidence")
        result["modules"].append(item)
    return result


def generate(rows: list[dict], manifest_path: Path, output: Path) -> None:
    lines = ["/* 由哈希已核验的 ACNH 3.0.0 模块生成；不要提交。 */"]
    for index, row in enumerate(rows):
        lines.append(f"static const Reloc reloc_{index}[] = {{")
        lines.extend(f"{{{r['where']}ULL,{r['kind']},{r['symbol']},{r['addend']}LL}}," for r in row["relocs"])
        lines.append("};")
        lines.append(f"static const Symbol symbol_{index}[] = {{")
        lines.extend("{" + f"{json.dumps(s['name'])},{s['value']}ULL,{s['size']}ULL,{int(s['defined'])},{s['binding']},{s['type']}" + "}," for s in row["symbols"])
        lines.append("};")
        lines.append(f"static const Gate gate_{index}[] = {{")
        gates_file = manifest_path.parent / f"{row['name']}.gates.tsv"
        for line in row["gates_text"].splitlines():
            if not line.strip():
                continue
            ret, kind, arg, original = line.split()
            require(kind in ("cntpct", "cntvct", "svc"), f"{gates_file}: unsupported gate {kind}")
            lines.append(f"{{{int(ret, 0)}ULL,{int(original, 0)}ULL,{int(kind != 'svc')},{int(arg)}}},")
        if not any(line.strip() for line in row["gates_text"].splitlines()):
            lines.append("{0,0,0,0},")
        lines.append("};")
    lines.append("static Module modules[] = {")
    for index, row in enumerate(rows):
        layout, tags = row["layout"], row["tags"]
        lines.append("{" + f".name={json.dumps(row['name'])},.digest={json.dumps(row['sha256'])},.file_size={row['size']}ULL,.text_end={layout['TEXT_END']}ULL,.ro_off={layout['RO_OFF']}ULL,.data_off={layout['DATA_OFF']}ULL,.patch_off={layout['PATCH_OFF']}ULL,.key_off={layout['KEY_PAGE_OFF']}ULL,.symbols=symbol_{index},.nsym={len(row['symbols'])},.relocs=reloc_{index},.nreloc={len(row['relocs'])},.gates=gate_{index},.ngate={sum(1 for x in row['gates_text'].splitlines() if x.strip())},.init={tags.get(DT_INIT, 0)}ULL,.ctor_flag={row['ctor_flag']}ULL,.init_array={tags.get(DT_INIT_ARRAY, 0)}ULL,.init_size={tags.get(DT_INIT_ARRAYSZ, 0)}ULL" + "},")
    lines.append("};")
    startup = rows[0]["startup_evidence"]
    lines.append(f"#define PROBE_PLT {rows[0]['probe_plt']}ULL\n#define PROBE_SLOT {rows[0]['probe_slot']}ULL")
    lines.append(f"#define ACNH_NNMAIN {startup['nnMain_offset']}ULL\n#define ACNH_NN_INIT_START {startup['nn_init_start_offset']}ULL")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")
    romfs_dir = ROOT / "work/acnh/3.0.0/romfs"
    index = json.loads((romfs_dir / "index.json").read_text())
    names = bytearray()
    entries = bytearray()
    for item in index["files"]:
        name = item["path"].encode("utf-8") + b"\0"
        entries.extend(struct.pack("<QQII", item["offset"], item["size"], len(names), len(name)))
        names.extend(name)
    runtime_index = struct.pack("<8sQQ", b"TOTKRFS1", len(index["files"]), 24 + len(entries)) + entries + names
    (romfs_dir / "runtime-index.bin").write_bytes(runtime_index)
    index_sha = sha256(runtime_index)
    config = (f'#define ROMFS_IMAGE_SIZE {index["romfs_size"]}ULL\n'
              f'#define ROMFS_IMAGE_SHA256 "{index["romfs_sha256"]}"\n'
              f'#define ROMFS_INDEX_SHA256 "{index_sha}"\n')
    (output.parent / "romfs_config.h").write_text(config)


def generate_shared_fonts_config() -> None:
    folder = ROOT / "work/acnh/3.0.0/shared-fonts"
    manifest_path = folder / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get("status") == "pass", "ACNH shared-font manifest has not passed validation")
    fonts = sorted(manifest.get("fonts", []), key=lambda item: item.get("type", -1))
    require([item.get("type") for item in fonts] == list(range(6)), "shared-font manifest must contain types 0 through 5")
    font_profile = json.loads((ROOT / "games/acnh/profiles/shared-fonts.json").read_text())
    require(font_profile.get("schema") == 1, "unsupported ACNH shared-font profile schema")
    expected = font_profile.get("fonts", [])
    expected = sorted(expected, key=lambda item: item.get("type", -1))
    require(len(expected) == 6 and [item.get("type") for item in expected] == list(range(6)),
            "ACNH shared-font profile must contain types 0 through 5")
    rows = []
    for font, pinned in zip(fonts, expected):
        for field in ("type", "title_id", "bfttf_bytes", "font_bytes", "sha256"):
            require(font.get(field) == pinned.get(field), f"shared-font {field} differs from the pinned profile for type {pinned['type']}")
        filename = font.get("file")
        require(isinstance(filename, str) and filename and "\\" not in filename,
                f"unsafe shared-font path for type {pinned['type']}")
        relative_path = Path(filename)
        require(not relative_path.is_absolute() and ".." not in relative_path.parts,
                f"unsafe shared-font path for type {pinned['type']}")
        path = folder / relative_path
        resolved_path = path.resolve()
        require(resolved_path.is_relative_to(folder.resolve()), f"shared-font path escapes its directory: {filename}")
        require(resolved_path.is_file(), f"shared-font file is missing: {filename}")
        data = resolved_path.read_bytes()
        require(len(data) == pinned["bfttf_bytes"], f"shared font size mismatch: {filename}")
        require(sha256(data) == pinned["sha256"], f"shared font SHA-256 mismatch: {filename}")
        require(pinned["bfttf_bytes"] == pinned["font_bytes"] + 8,
                f"shared font BFTTF size mismatch: {filename}")
        path_literal = json.dumps(str(resolved_path), ensure_ascii=False)
        rows.append(f'{{{path_literal},{pinned["bfttf_bytes"]}ULL,{pinned["font_bytes"]}ULL,"{pinned["sha256"]}"}}')
    output = ROOT / "work/acnh/3.0.0/build/shared_fonts_config.h"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(f"#define SHARED_FONT_COUNT {len(rows)}\n#define SHARED_FONT_ROWS " + ",".join(rows) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("analyze", "tables", "boot-check", "fonts-config"))
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path, default=ROOT / "work/acnh/3.0.0/build/runtime_tables.h")
    args = parser.parse_args()
    if args.mode == "fonts-config":
        generate_shared_fonts_config()
        print(json.dumps({"status": "pass", "fonts": 6,
                          "config": "work/acnh/3.0.0/build/shared_fonts_config.h"}, indent=2))
        return 0
    manifest = args.manifest.resolve()
    rows = inspect(manifest)
    if args.mode == "boot-check":
        gate_path = ROOT / "work/acnh/3.0.0/verification.json"
        gate = json.loads(gate_path.read_text())
        require(gate.get("status") == "pass", "structural verification has not passed")
        require(gate.get("rewrite_manifest_sha256") == sha256(manifest.read_bytes()),
                "structural verification is for a different rewrite manifest")
        module_proofs = gate.get("modules", {})
        require(set(module_proofs) == set(MODULES), "structural verification must cover main, subsdk0, and sdk")
        for name, proof in module_proofs.items():
            for key, filename in (("structure_log_sha256", f"{name}-structure.log"),
                                  ("equivalence_log_sha256", f"{name}-equivalence.log")):
                log = ROOT / "work/acnh/3.0.0/verification" / filename
                require(log.is_file() and sha256(log.read_bytes()) == proof.get(key),
                        f"{name}: structural verification log missing or hash mismatch ({key})")
        generate_shared_fonts_config()
        print(json.dumps({"status": "pass", "rewrite_manifest_sha256": gate["rewrite_manifest_sha256"],
                          "modules": list(MODULES)}, indent=2))
        return 0
    if args.mode == "analyze":
        report = disassemble_evidence(rows, manifest)
        output = ROOT / "work/acnh/3.0.0/analysis/runtime-entry-ctor.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n")
        proof = {"status": "pass", "rewrite_manifest_sha256": sha256(manifest.read_bytes()),
                 "romfs_sha256": read_profile()["romfs"]["sha256"],
                 "entry_ctor_report": str(output.relative_to(ROOT)),
                 "entry_ctor_report_sha256": sha256(output.read_bytes())}
        proof_path = ROOT / "work/acnh/3.0.0/verification/runtime-entry-ctor.json"
        proof_path.parent.mkdir(parents=True, exist_ok=True)
        proof_path.write_text(json.dumps(proof, indent=2) + "\n")
        entry_ok = len(rows[0].get("entry_candidates", [])) == 1
        start_ok = len(rows[2].get("start_candidates", [])) == 1
        status = "pass" if entry_ok and start_ok else "unsupported-shape"
        print(json.dumps({"status": status, "report": str(output.relative_to(ROOT)),
                          "entry_candidates": len(rows[0].get("entry_candidates", [])),
                          "sdk_start_exports": len(rows[2].get("start_candidates", [])),
                          "constructor_shapes": {row["name"]: row["ctor_evidence"] for row in rows}}, indent=2))
        return 0 if entry_ok and start_ok else 2
    proof = ROOT / "work/acnh/3.0.0/verification/runtime-entry-ctor.json"
    proof_data = json.loads(proof.read_text())
    require(proof_data.get("status") == "pass", "entry/ctor verification proof is missing or failed; run make analyze")
    require(proof_data.get("rewrite_manifest_sha256") == sha256(manifest.read_bytes()),
            "entry/ctor proof is for a different rewrite manifest")
    require(proof_data.get("romfs_sha256") == read_profile()["romfs"]["sha256"],
            "entry/ctor proof is for a different merged RomFS")
    report_path = ROOT / proof_data["entry_ctor_report"]
    require(sha256(report_path.read_bytes()) == proof_data.get("entry_ctor_report_sha256"),
            "entry/ctor report hash mismatch")
    generate(rows, manifest, args.output.resolve())
    print(json.dumps({"status": "pass", "tables": str(args.output), "romfs_index": str((ROOT / "work/acnh/3.0.0/romfs/runtime-index.bin").relative_to(ROOT)), "modules": len(rows),
                      "relocations": sum(len(row["relocs"]) for row in rows)}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeErrorDetail, OSError, KeyError, ValueError, struct.error) as exc:
        print(f"prepare_runtime: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
