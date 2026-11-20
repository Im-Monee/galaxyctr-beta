"""Wii release metadata used by GalaxyCTR's builder and file converter."""

from dataclasses import dataclass


@dataclass(frozen=True)
class GameVersion:
    game_id: str
    revision: int
    name: str
    main_dol_sha1: str
    port_data: tuple[tuple[str, int, int], ...]


# Petari's RMGJ01 and RMGE01 symbols.txt place these assets at the same addresses/sizes.
_PORT_DATA = (
    ("StoryProgressTable.bcsv", 0x8053C620, 0x1E0),
    ("GalaxyInfoTable.bcsv", 0x8053C800, 0xD20),
    ("ErrorMessageArchive.arc", 0x8053DFE0, 0x9AC0),
)

SUPPORTED_VERSIONS = {
    "RMGE01": GameVersion("RMGE01", 0, "USA", "9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd", _PORT_DATA),
    "RMGJ01": GameVersion("RMGJ01", 0, "Japan", "604d0b47d446caec0aab9ac370014f132e1b4016", _PORT_DATA),
}


def get_game_version(game_id: str) -> GameVersion | None:
    """Return metadata for a supported disc ID, or None when the release is unsupported."""
    return SUPPORTED_VERSIONS.get(game_id)


def game_version_for_main_dol_sha1(digest: str) -> GameVersion | None:
    """Identify a supported release from a main.dol checksum."""
    digest = digest.lower()
    return next((version for version in SUPPORTED_VERSIONS.values() if version.main_dol_sha1 == digest), None)
