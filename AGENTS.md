# Repository instructions

## Build and verification

- Prefer the Docker-based build for compiling and validating RamTool. Do not run
  `build.ps1` directly on the host unless the task specifically requires testing
  the host toolchain path.
- Build the image and verify its artifacts with:

  ```sh
  docker compose build
  docker compose run --rm tools
  ```

  The Dockerfile runs the repository's `build.ps1` with its pinned ESP32
  toolchain. Compose exposes `build/` on the host so the generated artifacts can
  be used with the host loader.
- On Windows and macOS, use Docker for building and run `run.py` on the host to
  access serial hardware. Container serial passthrough is not reliable on those
  platforms. See `docker-compose.yml` for Linux and Windows-over-TCP options.
- When changing packaging or the standalone distribution, also check the
  generated `dist/` files and confirm `dist/run.py` can locate its payload.
- Do not commit generated `build/` or `dist/` outputs.
