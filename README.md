# 14CFW
Custom firmware installer for Nintendo Switch (Atmosphère + Hekate + homebrew), run as an NRO.

Install: put `14CFW.nro` in `sd:/switch/14CFW/` and launch it from hbmenu.

What it does
- Scans the SD card and shows what is already installed (Atmosphère, Hekate, homebrew).
- Lets you pick components, downloads the latest releases from GitHub, extracts them to the SD.
- Backs up every file it would overwrite to `switch/14CFW/backup/<time>/`.
- Keeps your existing config files (ini/json/cfg) when "Keep my existing settings" is on.
- Never writes to `Nintendo/`, `emuMMC/`, `backup/`, `switch/prod.keys`, `switch/title.keys`: games, saves and emuMMC stay untouched.
- Installs a custom 14CFW Hekate boot menu (logo, icon, entries). Old `hekate_ipl.ini` is backed up first.

Customize: create `sd:/switch/14CFW/manifest.json` to add or change components (see the default list in installer.cpp).
