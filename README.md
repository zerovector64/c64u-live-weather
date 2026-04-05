# C64U Live Weather

This project is a *proof of concept* **Commodore 64 Ultimate live-data viewer** that receives updates directly in RAM via the Ultimate REST API.

> **Status:** This project still needs a lot of work and is far from a finished product. Expect rough edges, missing features, and ongoing changes as development continues.

## What it does

- draws a simple PETSCII dashboard with bars and status text
- reads the live packet from a fixed memory buffer on the C64
- receives updates from the PC through `PUT /v1/machine:writemem`
- keeps the location and API defaults inside `php/live_bridge.php`

## PC-side bridge

The PHP bridge fetches weather data from **Open-Meteo** and pushes it to the Ultimate:

```powershell
php php/live_bridge.php --mode mem --ultimate-ip 192.168.1.64
```

Useful options:

- `--once` → fetch once and push once
- `--interval 900` → refresh every 15 minutes (default)
- `--ultimate-ip 192.168.4.64` → target the Ultimate device
- `--temp-unit F` → switch to Fahrenheit instead of Celsius
- `--location`, `--lat`, `--lon` → optional overrides if you want to change the PHP defaults

## How to use it

1. Run `build and run on c64 ultimate` to deploy and start the PRG.
2. Run `live_bridge.php` with option `--once` to test.
3. Run `start live_bridge.php` for continuous updates.
4. If you want different weather/location defaults, configure the options.


