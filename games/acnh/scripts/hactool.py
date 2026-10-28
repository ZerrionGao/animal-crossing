"""构建处理 XCI/NSP 的固定版本 hactool。"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

HACTOOL = ("https://github.com/SciresM/hactool.git", "1d64a83450e025622f3468c28fc4164dad2c5ef6")


class HactoolBuildError(RuntimeError):
    pass


def _run(command: list[str], log: Path, cwd: Path) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = "\n".join(log.read_text(errors="replace").splitlines()[-25:])
        raise HactoolBuildError(f"{Path(command[0]).name} failed (exit {result.returncode}); log: {log}\n{tail}")


def _fetch(url: str, commit: str, destination: Path, log: Path) -> None:
    if (destination / ".git").is_dir():
        revision = subprocess.run(
            ["git", "-C", str(destination), "rev-parse", "HEAD"], capture_output=True, text=True
        ).stdout.strip()
        if revision == commit:
            return
    if destination.is_symlink():
        destination.unlink()
    elif destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    _run(["git", "init", "-q"], log, destination)
    _run(["git", "fetch", "-q", "--depth", "1", url, commit], log, destination)
    _run(["git", "checkout", "-q", "FETCH_HEAD"], log, destination)


def build_hactool(work: Path, logs: Path) -> Path:
    """Fetch the pinned source, apply the ACNH-safe key and bounded-file changes, and build it."""
    source = work / "src/hactool"
    _fetch(*HACTOOL, source, logs / "fetch-hactool.log")

    main = source / "main.c"
    text = main.read_text()
    old = 'FILE *titlekeyfile = open_key_file("title");'
    new = 'FILE *titlekeyfile = getenv("MNG_TITLE_KEYS") ? fopen(getenv("MNG_TITLE_KEYS"), "rb") : open_key_file("title");'
    if new not in text:
        if text.count(old) != 1:
            raise HactoolBuildError("pinned hactool title-key hook changed")
        main.write_text(text.replace(old, new))

    text = main.read_text()
    if "#define _DARWIN_C_SOURCE 1" not in text:
        text = "#define _DARWIN_C_SOURCE 1\n" + text
        main.write_text(text)
    if "static FILE *mng_open_base(const char *path)" not in text:
        helper = r'''
typedef struct { FILE *file; off_t start, size, position; } mng_base_range;
static int mng_base_read(void *cookie, char *buffer, int count) {
    mng_base_range *range = cookie;
    off_t left = range->size - range->position;
    if (left < count) count = (int)left;
    if (!count) return 0;
    if (fseeko(range->file, range->start + range->position, SEEK_SET)) return -1;
    int got = (int)fread(buffer, 1, count, range->file);
    if (!got && ferror(range->file)) return -1;
    range->position += got;
    return got;
}
static fpos_t mng_base_seek(void *cookie, fpos_t offset, int whence) {
    mng_base_range *range = cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;
    if (base < 0 || offset < -base || offset > range->size - base) return (fpos_t)-1;
    return (fpos_t)(range->position = base + offset);
}
static int mng_base_close(void *cookie) {
    mng_base_range *range = cookie;
    int result = fclose(range->file); free(range); return result;
}
static FILE *mng_open_base(const char *path) {
    const char *start = getenv("MNG_BASE_RANGE_OFFSET"), *size = getenv("MNG_BASE_RANGE_SIZE");
    if (!start && !size) return fopen(path, "rb");
    if (!start || !size) return NULL;
    char *end1, *end2;
    unsigned long long a = strtoull(start, &end1, 10), n = strtoull(size, &end2, 10);
    if (!*start || !*size || *end1 || *end2 || a > INT64_MAX || !n || n > INT64_MAX - a) return NULL;
    mng_base_range *range = calloc(1, sizeof(*range));
    if (!range) return NULL;
    range->file = fopen(path, "rb"); range->start = (off_t)a; range->size = (off_t)n;
    if (!range->file) { free(range); return NULL; }
    if (fseeko(range->file, 0, SEEK_END) || ftello(range->file) < (off_t)(a + n)) { mng_base_close(range); return NULL; }
    FILE *view = funopen(range, mng_base_read, NULL, mng_base_seek, mng_base_close);
    if (!view) mng_base_close(range);
    return view;
}
'''
        marker = "int main(int argc, char **argv)"
        original = 'nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")'
        if text.count(marker) != 1 or text.count(original) != 2:
            raise HactoolBuildError("pinned hactool bounded base-file hook changed")
        text = text.replace(marker, helper + "\n" + marker)
        text = text.replace(original, 'nca_ctx.tool_ctx->base_file = mng_open_base(optarg)')
        main.write_text(text)

    nca = source / "nca.c"
    text = nca.read_text()
    old = (
        "sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n"
        "            sb->subsection_header.offset + sb->subsection_header.size != ctx->size"
    )
    new = (
        "sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n"
        "            sb->subsection_header.offset + sb->subsection_header.size > ctx->size ||\n"
        "            (ctx->size - (sb->subsection_header.offset + sb->subsection_header.size)) % 0x200 != 0"
    )
    if new not in text:
        if text.count(old) != 1:
            raise HactoolBuildError("pinned hactool BKTR layout check changed")
        text = text.replace(old, new)
        nca.write_text(text)
    old = "if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {"
    new = (
        "if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset &&\n"
        "            (sb->relocation_header.size > sb->subsection_header.offset ||\n"
        "             ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset - sb->relocation_header.size)) {"
    )
    if new not in text:
        if text.count(old) != 1:
            raise HactoolBuildError("pinned hactool BKTR total-size check changed")
        nca.write_text(text.replace(old, new))

    config = source / "config.mk"
    if not config.exists():
        shutil.copy2(source / "config.mk.template", config)
    _run(["make", "-j2", "CC=clang"], logs / "hactool-build.log", source)
    return source / "hactool"
