import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"
SCRIPT = ROOT / "games/acnh/scripts/prepare_runtime.py"
spec = importlib.util.spec_from_file_location("acnh_prepare_runtime", SCRIPT)
prepare_runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_runtime)


class AcnhRuntimeTests(unittest.TestCase):
    def test_acnh_application_id_is_shared_by_program_and_save_checks(self):
        services = (RUNTIME / "services.c").read_text()
        fs_host = (RUNTIME / "fs_host.c").read_text()
        header = (RUNTIME / "game_application_id.h").read_text()
        self.assertIn("#define GAME_PROGRAM_ID GAME_APPLICATION_ID", services)
        self.assertIn("#define TOTK_APPLICATION_ID GAME_APPLICATION_ID", fs_host)
        self.assertIn("#define GAME_APPLICATION_ID 0x01006f8002326000ULL", header)
        self.assertIn("_Static_assert(GAME_APPLICATION_ID == 0x01006f8002326000ULL", header)

    def test_generated_shared_font_paths_are_absolute(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            folder = root / "work/acnh/3.0.0/shared-fonts"
            folder.mkdir(parents=True)
            fonts = []
            for font_type in range(6):
                filename = f"{font_type}-font.bfttf"
                data = b"BFTTF123font"
                (folder / filename).write_bytes(data)
                fonts.append({
                    "type": font_type,
                    "title_id": f"01000000000008{font_type:02x}",
                    "file": filename,
                    "bfttf_bytes": len(data),
                    "font_bytes": len(data) - 8,
                    "sha256": prepare_runtime.sha256(data),
                })
            (folder / "manifest.json").write_text(json.dumps({"status": "pass", "fonts": fonts}))
            profile = root / "games/acnh/profiles/shared-fonts.json"
            profile.parent.mkdir(parents=True)
            profile.write_text(json.dumps({"schema": 1, "fonts": [
                {key: font[key] for key in ("type", "title_id", "bfttf_bytes", "font_bytes", "sha256")}
                for font in fonts
            ]}))
            with patch.object(prepare_runtime, "ROOT", root):
                prepare_runtime.generate_shared_fonts_config()
            config = (root / "work/acnh/3.0.0/build/shared_fonts_config.h").read_text()
            for font in fonts:
                self.assertIn(f'"{(folder / font["file"]).resolve()}"', config)

            fonts[0]["sha256"] = "0" * 64
            (folder / "manifest.json").write_text(json.dumps({"status": "pass", "fonts": fonts}))
            with patch.object(prepare_runtime, "ROOT", root):
                with self.assertRaisesRegex(prepare_runtime.RuntimeErrorDetail, "sha256 differs from the pinned profile"):
                    prepare_runtime.generate_shared_fonts_config()

            fonts[0]["sha256"] = json.loads(profile.read_text())["fonts"][0]["sha256"]
            fonts[0]["file"] = "../outside.bfttf"
            (folder / "manifest.json").write_text(json.dumps({"status": "pass", "fonts": fonts}))
            with patch.object(prepare_runtime, "ROOT", root):
                with self.assertRaisesRegex(prepare_runtime.RuntimeErrorDetail, "unsafe shared-font path"):
                    prepare_runtime.generate_shared_fonts_config()

    @unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
    def test_acnh_application_id_compile_time_assertion(self):
        source = '#include "game_application_id.h"\nint main(void) { return GAME_APPLICATION_ID != 0x01006f8002326000ULL; }\n'
        result = subprocess.run(
            ["xcrun", "clang", "-std=c11", "-DGAME_ACNH", "-I", str(RUNTIME), "-fsyntax-only", "-x", "c", "-"],
            input=source,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
