# Fan control for TerraMaster on Linux

Tested with F4-424 Pro. This is a direct port of the Xpenology fancontrol script by Eudean to work on OMV/Debian.

Original author: https://xpenology.com/forum/topic/14007-terramaster-f4-220-fan-control/?ct=1559481439

This fork implements changes for it to work with NAS devices containing the IT8613E chipset, while the original program only supported IT8772E (used in the F4-220).
Initially I made the changes described in [this post](https://xpenology.com/forum/topic/14007-terramaster-f4-220-fan-control/?do=findComment&comment=264172), but in the end I just commented out the part that was specific for the IT8772E.

## Features

1. **Configuration file support** - All settings can be configured via `/etc/fancontrol.conf`
2. **Auto-detection of drives** - Automatically detects HDDs, SSDs, and NVMe drives
3. **Silent temperature readings, no dependencies** - By default all drive temperatures come from the kernel's hwmon sensors (`drivetemp` for SATA/SAS, `nvme` for NVMe): no `smartmontools`, `lm-sensors` or `nvme-cli`, and no HDD click on every poll. See [Drive Temperature Source](#drive-temperature-source)
4. **NVMe support** - Full support for NVMe drive temperature monitoring
5. **Standby-aware** - With `temp_source = smart`, sleeping SATA drives aren't woken for temperature checks
6. **Simple fan curve** - Linear fan speed control between two temperature thresholds
7. **Monitor mode** - `--monitor-only` prints temperatures and the computed fan speed without touching the hardware
8. **Graphite integration** - Optional reporting to a Graphite server for monitoring in Grafana (auto-reconnects)
9. **Validated configuration** - Bad config values are rejected or warned about instead of crashing the daemon

## Installation:
Warning: As from Truenas 24.10.1, [the home folder is no longer executable](https://forums.truenas.com/t/shell-script-permission-denied-with-24-10-1/27941). Instead, use the data pool for your scripts.

### Prerequisites

With the default `temp_source = hwmon` there is nothing to install — no `smartmontools`, `lm-sensors` or `nvme-cli`. Temperatures come straight from the kernel. The only requirement is the `drivetemp` module for SATA/SAS drives (NVMe sensors come with the regular `nvme` driver):

```bash
lsmod | grep drivetemp || sudo modprobe drivetemp
```

TrueNAS SCALE already loads it for its own disk temperature reporting. On other distributions, load it on every boot:

```bash
echo drivetemp | sudo tee /etc/modules-load.d/drivetemp.conf
```

Only if you switch to [`temp_source = smart`](#drive-temperature-source) do you need the userspace tools:

```bash
# For SATA/SAS drive temperature monitoring
apt install smartmontools lm-sensors

# For NVMe drive temperature monitoring (optional but recommended)
apt install nvme-cli
```

### Install from Release (no toolchain needed)

Each [release](https://github.com/schudt/terramaster-fancontrol-IT8613E/releases) ships `fancontrol-linux-x86_64.tar.gz` containing the static binary, sample config, systemd unit, and installer. On the NAS:

```bash
curl -sL https://github.com/schudt/terramaster-fancontrol-IT8613E/releases/latest/download/fancontrol-linux-x86_64.tar.gz | tar xz
sudo ./install_service.sh
```

The binary is fully static — runs on any x86_64 Linux, no library requirements.

### Build from Source

1. Clone the repo
   ```
   git clone https://github.com/schudt/terramaster-fancontrol-IT8613E
   ```

2. Build.
   - Directly (needs g++):
     ```
     make
     ```
   - Or with the GCC Docker image (no local toolchain needed):
     ```
     docker pull gcc
     sudo docker run --rm -v "$PWD":/usr/src/myapp -w /usr/src/myapp gcc g++ -O2 -Wall -static -s -o fancontrol fancontrol.cpp
     ```
     Note: use `g++` (not `gcc`) — the source is C++ and needs libstdc++ linked. Keep `-static`: the `gcc` image ships a much newer glibc than TrueNAS SCALE or Debian stable, so a dynamically linked binary fails there with `GLIBC_2.38 not found` / `GLIBCXX_3.4.31 not found`.

3. Optional: run the unit tests (works on Linux and macOS, no root needed):
   ```
   make test
   ```

### Quick Start (Auto-detection)

The easiest way to run fancontrol is with automatic drive detection:
```bash
sudo ./fancontrol --auto_detect --debug=1
```

This will automatically detect all HDDs, SSDs, and NVMe drives in your system.

To watch temperatures and the computed fan speed without touching the fans:
```bash
sudo ./fancontrol --monitor-only
```

### Configuration File

1. Generate a sample configuration file:
   ```bash
   sudo ./fancontrol --generate-config
   ```
   This creates `/etc/fancontrol.conf` with default settings.

2. Edit the configuration file to your needs:
   ```bash
   sudo nano /etc/fancontrol.conf
   ```

3. Run with config file:
   ```bash
   sudo ./fancontrol --config=/etc/fancontrol.conf
   ```

### Manual Drive Specification

If you prefer to manually specify drives:
```bash
sudo ./fancontrol --drive_list="sda,sdb,sdc,sdd,nvme0n1" --debug=1
```

### Systemd Service Installation

Easiest: build + install + start in one go (wraps `install_service.sh`):
```bash
sudo make install
```

Or manually:

1. Copy the binary and config:
   ```bash
   sudo cp fancontrol /usr/local/bin/
   sudo cp fancontrol.conf /etc/
   ```

2. Copy and enable the service:
   ```bash
   sudo cp fancontrol.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl start fancontrol.service
   sudo systemctl enable fancontrol.service
   ```

3. Check status:
   ```bash
   sudo systemctl status fancontrol.service
   ```

Note: You may need to reinstall the service after Truenas updates. Use `install_service.sh` for convenience.

## Parameters:
```
Usage:

 fancontrol [--config=<path>] [--auto_detect] [options]

Configuration:
  --config=<path>       Path to config file (default: /etc/fancontrol.conf)
  --generate-config     Generate a sample config file and exit

Drive Options:
  --drive_list=<list>   Comma-separated list of drive names e.g. 'sda,nvme0n1'
  --auto_detect         Auto-detect drives (default)
  --no_nvme             Exclude NVMe drives from auto-detection
  --no_hdd              Exclude HDD/SSD drives from auto-detection
  --temp_source=<hwmon|smart>  Where drive temperatures come from (default: hwmon)
                        hwmon: kernel sensors only (drivetemp, nvme), no tools
                        smart: smartctl (SATA/SAS), hwmon or nvme-cli (NVMe)

Fan Curve:
  --temp_low=<value>    Temperature for minimum fan speed (default: 40°C)
  --temp_high=<value>   Temperature for maximum fan speed (default: 60°C)
  --fan_min=<value>     Minimum PWM, fans never go below (default: 80 ~30%)
  --fan_start=<value>   Fan PWM at temp_low (default: 100 ~40%)
  --fan_max=<value>     Maximum PWM at temp_high (default: 255 100%)

Other:
  --debug=<0|1>         Enable debug logging (default: 0)
  --interval=<value>    Polling interval in seconds (default: 10)
  --cpu_temp_offset=<value>  CPU temp offset vs drives (default: 20°C)
  --graphite_server=<ip:port>  Graphite server for metrics
  --monitor-only        Only display temps and fan speed, don't control fans
```

Config file settings are overridden by command line arguments.

## Fan Curve

The fan speed is controlled using a simple linear interpolation between temperature thresholds:

```
Fan Speed
   ^
100%|                     ________
    |                    /
    |                   /
 40%|__________________/
    |       ^         ^
 30%|-------|---------|----------
    +-------+---------+--------> Temperature
          40°C      60°C
       (temp_low) (temp_high)
```

- **Below temp_low (40°C)**: Fans run at `fan_min` (~30%) - quiet operation
- **At temp_low (40°C)**: Fans run at `fan_start` (~40%)
- **Between temp_low and temp_high**: Linear interpolation
- **At temp_high (60°C) and above**: Fans run at `fan_max` (100%)

Example with default settings:
| Temperature | Fan Speed |
|-------------|-----------|
| 35°C        | 31%       |
| 40°C        | 39%       |
| 50°C        | 70%       |
| 60°C+       | 100%      |

## Configuration File Format

The configuration file uses INI-style format with sections:

```ini
[general]
debug = false
interval = 10

[drives]
auto_detect = true
include_nvme = true
include_hdd = true
# Where drive temperatures come from: hwmon (default) or smart
temp_source = hwmon
# Don't wake sleeping SATA drives for temperature checks (smart only)
respect_standby = true
# drive_list = sda,sdb,sdc,sdd,nvme0n1

[fan_curve]
# Temperature thresholds (Celsius)
temp_low = 40
temp_high = 60

# Fan speed limits (PWM: 0-255)
fan_min = 80      # ~31% - minimum speed
fan_start = 100   # ~39% - speed at temp_low
fan_max = 255     # 100% - speed at temp_high

[cpu]
cpu_temp_offset = 20
cpu_avg_samples = 10

[graphite]
# graphite_server = 192.168.1.100:2003
```

## Drive Auto-Detection

The program automatically detects drives by scanning `/sys/block/`. It identifies:
- **HDDs** - Rotational drives (detected via `/sys/block/<dev>/queue/rotational`)
- **SSDs** - Non-rotational SATA drives
- **NVMe** - NVMe drives (detected by device name starting with `nvme`)

How the temperature of each drive is read depends on `temp_source`, see below.

## Drive Temperature Source

`temp_source` in the `[drives]` section (or `--temp_source=` on the command line) selects how drive temperatures are read:

| | `hwmon` (default) | `smart` |
|---|---|---|
| SATA/SAS | kernel `drivetemp` sensor | `smartctl -A` |
| NVMe | kernel `nvme` sensor | hwmon, falling back to `nvme smart-log` |
| External tools | none | `smartmontools`, `nvme-cli`, `lm-sensors` |
| HDD noise per poll | silent | may click (see below) |
| Sensor unreadable | fans go to full speed | drive counts as 0°C |
| Sleeping drives | may be kept awake | left alone (`respect_standby`) |

### Why `hwmon` is the default

`smartctl -A` reads the SMART attribute table, which many HDDs keep in a reserved area on the platters. Every poll moves the heads there and back, so with a 10 s interval you hear a short click from every drive. It's noise, not wear (no extra load/unload cycles), but in a living room it's noticeable.

The kernel's `drivetemp` driver asks the drive through SCT Command Transport instead, which is answered by the drive electronics without moving the heads. (Only drives without SCT support fall back to reading SMART attributes.) NVMe drives expose their sensor through the regular `nvme` driver. Both show up under `/sys/class/hwmon/`, which is also where TrueNAS SCALE gets the disk temperatures for its reporting.

On top of being quiet, this needs no external tools at all: the daemon reads sysfs and spawns no processes.

### How it behaves

- Each drive is matched to its sensor through the device it belongs to, on every poll. hwmon numbers (`hwmon5`, `hwmon6`, …) follow driver load order, not drive names, and can change after a reboot or update, so they're never used for the mapping.
- If a drive's sensor can't be found or read (for example because `drivetemp` isn't loaded), fancontrol logs a warning and runs the fans at **full speed** until the sensor is back. There is no silent fallback to `smartctl`.
- `respect_standby` has no effect: the kernel driver is queried on every poll. According to the `drivetemp` documentation, reading the temperature can reset the spin-down timer on some drives, so drives configured to spin down may stay awake.
- Because the reads are cheap and silent, a short `interval` (the default 10 s) is fine.

### When to use `smart` instead

Set this in `/etc/fancontrol.conf` if `drivetemp` isn't available on your system, or if your drives are meant to spin down and stay asleep:

```ini
[drives]
temp_source = smart
```

Check which source is active either way — the startup line says so:

```bash
sudo fancontrol --monitor-only
```

### Upgrading from a version without `temp_source`

An existing `/etc/fancontrol.conf` has no `temp_source` line, so it now uses `hwmon`. If `drivetemp` isn't loaded on your system, the fans will run at full speed and the log will say so on the first poll. Either load the module (see [Prerequisites](#prerequisites)) or add `temp_source = smart` to keep the previous behaviour.
