# Eleven Rack audio driver — how it lives in this repo

This folder is a `git subtree` of https://github.com/damielolar-CODE/eleven-rack-driver,
a fork of Matt Housley's https://github.com/Matt-Housley/eleven-rack-driver (MIT).
His `LICENSE` file stays here unchanged and applies to everything under `driver/`.

- **Credit:** Matt Housley wrote the driver (HAL plug-in, USB engine, menu-bar app).
  Eleven Edit's changes are listed at the top of `History.txt` under 1.2.0.
- **Build the installer:** `npm run build:driver` from the repo root (wraps
  `driver/packaging/build_pkg.sh`; output `dist/driver/ElevenRackDriver-<version>.pkg`).
- **Test the playback consumer without hardware:** `npm run test:driver`
  (`ElevenRackBridge/tests/erplay_sim.c`).
- **Pull upstream changes:** `git subtree pull --prefix driver https://github.com/damielolar-CODE/eleven-rack-driver.git main --squash`
- **Push our changes back to the fork:** `git subtree push --prefix driver https://github.com/damielolar-CODE/eleven-rack-driver.git main`
