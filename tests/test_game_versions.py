import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "3ds" / "builder"))
sys.path.insert(0, str(ROOT / "tools" / "3ds"))

import game_versions
import convert_smg_files


class GameVersionTests(unittest.TestCase):
    def test_supported_disc_ids_have_distinct_dol_checksums(self):
        usa = game_versions.get_game_version("RMGE01")
        japan = game_versions.get_game_version("RMGJ01")
        self.assertIsNotNone(usa)
        self.assertIsNotNone(japan)
        self.assertEqual(usa.revision, 0)
        self.assertEqual(japan.revision, 0)
        self.assertNotEqual(usa.main_dol_sha1, japan.main_dol_sha1)
        self.assertEqual(game_versions.game_version_for_main_dol_sha1(japan.main_dol_sha1), japan)
        self.assertIsNone(game_versions.get_game_version("RMGP01"))

    def test_supported_releases_share_petari_symbol_backed_port_data(self):
        usa = game_versions.get_game_version("RMGE01")
        japan = game_versions.get_game_version("RMGJ01")
        self.assertEqual(usa.port_data, japan.port_data)
        self.assertEqual(usa.port_data, (
            ("StoryProgressTable.bcsv", 0x8053C620, 0x1E0),
            ("GalaxyInfoTable.bcsv", 0x8053C800, 0xD20),
            ("ErrorMessageArchive.arc", 0x8053DFE0, 0x9AC0),
        ))

    def test_dol_section_extraction_uses_load_address_and_checks_file_bounds(self):
        dol = bytearray(0x120)
        file_offset, load_address, size = 0x100, 0x8053C000, 0x20
        dol[0:4] = file_offset.to_bytes(4, "big")
        dol[0x48:0x4C] = load_address.to_bytes(4, "big")
        dol[0x90:0x94] = size.to_bytes(4, "big")
        payload = bytes(range(size))
        dol[file_offset:file_offset + size] = payload
        self.assertEqual(convert_smg_files._dol_bytes(dol, load_address + 4, 8), payload[4:12])
        with self.assertRaises(ValueError):
            convert_smg_files._dol_bytes(dol, load_address, size + 1)


if __name__ == "__main__":
    unittest.main()
