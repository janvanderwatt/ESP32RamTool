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

## Keeping the shipped instructions in sync

The distribution ships an `AGENTS.md` that lists every command, so it goes stale
the moment the command surface moves. When a command or user-visible behaviour
is added, changed, or removed, update all of these in the same change:

- `packaging/AGENTS.md`, which `build.ps1` copies verbatim to `dist/AGENTS.md`;
- the `help()` text in `payload.c`, which is what the payload itself prints;
- the Commands section of `README.md`.

Then rebuild and read the shipped copy back, for example:

```sh
docker compose build
docker run --rm ramtool cat dist/AGENTS.md
```
