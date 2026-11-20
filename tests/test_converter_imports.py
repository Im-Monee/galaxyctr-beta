import importlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "3ds"))


class ConverterImportTests(unittest.TestCase):
    """The converter is one file with long embedded bundles; an edit can drop a top-level import
    and only fail at conversion time, so check the modules it uses at run time are still bound."""

    def test_converter_module_imports_its_runtime_dependencies(self):
        module = importlib.import_module("convert_smg_files")
        for name in ("zipfile", "Counter", "argparse", "base64", "hashlib", "io", "json", "os",
                     "shutil", "struct", "subprocess", "sys", "tarfile", "tempfile", "threading",
                     "time", "urllib", "Path"):
            self.assertTrue(hasattr(module, name), f"convert_smg_files lost its {name} import")


if __name__ == "__main__":
    unittest.main()
