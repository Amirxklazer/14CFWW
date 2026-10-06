# 14CFW

Windows 10 OOBE-style installer for Atmosphère + Hekate + homebrew.

Put 14CFW.nro in `sd:/switch/14CFW/`, open it from the Homebrew Menu, follow the pages.

- Express: everything. Customize: pick Atmosphère / Hekate / boot menu / splash / each app.
- "Already installed" (on the "what we found" page): update only what is already on your SD card.
- Settings (X on the first page): backups, replace-or-add boot menu, 14 splash, recommended Atmosphère settings, auto boot + wait time, restore your old boot menu. Saved in `switch/14CFW/settings.ini`.
- Never touches Nintendo/, emuMMC/ or your keys. Existing configs are kept; replaced boot files are backed up to `switch/14CFW/backup/`.
