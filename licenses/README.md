# Third-party licenses

The builder executables carry these programs and libraries, unchanged.
Their license texts are in this folder and inside every builder executable.

| Component | Project | Version | License | Files |
|---|---|---|---|---|
| nodtool | [encounter/nod-rs](https://github.com/encounter/nod-rs) | v2.0.0-alpha.12 | MIT or Apache-2.0, at your choice | `nodtool-LICENSE-MIT.txt`, `nodtool-LICENSE-APACHE.txt` |
| makerom | [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR) | makerom-v0.19.0 | MIT | `makerom-LICENSE.txt` |
| bannertool | [diasurgical/bannertool](https://github.com/diasurgical/bannertool) | 1.2.0 (built from its source on macOS) | MIT | `bannertool-LICENSE.txt` |
| Python | [python.org](https://www.python.org/) | 3.12 | PSF License | `python-LICENSE.txt` |
| certifi | [certifi/python-certifi](https://github.com/certifi/python-certifi) | 2026.7.22 | MPL-2.0 | `certifi-LICENSE.txt` |

The executables are packed with [PyInstaller](https://pyinstaller.org/). Its bootloader is
GPL-2.0 with an exception that allows packed programs under any license.

The versions are the ones `.github/workflows/builder.yml` downloads and installs.
