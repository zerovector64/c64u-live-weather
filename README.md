# C64U Live Weather

This project is a *proof of concept* **Commodore 64 live-data viewer** that can receive updates either directly in RAM via the Ultimate REST API or through a Meatloaf-friendly IEC/TCP packet source.

> **Status:** This project still needs a lot of work and is far from a finished product. Expect rough edges, missing features, and ongoing changes as development continues.

## Two ways to run it

This project supports **two separate modes**:

- **Meatloaf mode** — the C64 talks to **Open-Meteo** through Meatloaf directly. 
- **Ultimate mode** — a PC-side PHP bridge fetches the weather data and pushes it into Ultimate RAM.

---

## Meatloaf mode

Use this mode when running the PRG from Meatloaf and you want the C64 to fetch weather data directly.

### How it works

- the app talks to `api.open-meteo.com` through Meatloaf
- your chosen location and units persist between runs in `live-weather.cfg`.

### Meatloaf config file

Example contents of `live-weather.cfg`:

```ini
LOCATION=NEW YORK
LAT=40.7128
LON=-74.0060
TEMP_UNIT=F
```

Supported keys:

- `LOCATION`
- `LAT` or `LATITUDE`
- `LON` or `LONGITUDE`
- `TEMP_UNIT` or `UNITS` (`C` or `F`)

### Meatloaf quick start

1. Start the PRG on the C64.
2. Use the setup screen to change location or units as needed.
3. The app saves those settings to `live-weather.cfg` automatically.

---

## Ultimate mode (requires PHP bridge)

Use this mode when you want a PC to fetch weather data and push live packets into the Ultimate memory buffer.

### How it works

- the PHP bridge fetches weather data from **Open-Meteo**
- it pushes updates through `PUT /v1/machine:writemem`
- the C64 reads the packet from the fixed RAM buffer

### PHP bridge

```powershell
php php/live_bridge.php --mode mem --ultimate-ip 192.168.1.64
```

Useful options:

- `--once` → fetch once and push once
- `--interval 900` → refresh every 15 minutes (default)
- `--ultimate-ip 192.168.4.64` → target the Ultimate device
- `--temp-unit F` → switch to Fahrenheit instead of Celsius
- `--location`, `--lat`, `--lon` → override the PHP defaults

### Ultimate quick start

1. Start the PHP bridge on your PC.
2. Run `build and run on c64 ultimate`.
3. The C64 app will display the pushed live data from the RAM buffer.


