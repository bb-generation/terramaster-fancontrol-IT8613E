// MIT License

// Copyright (c) 2019 Eudean Sun

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#ifdef __linux__
#include <sys/io.h>
#else
// Stubs so the pure logic can be unit-tested on non-Linux hosts
static inline void outb(unsigned char, unsigned short) {}
static inline unsigned char inb(unsigned short) { return 0; }
static inline int iopl(int) { return -1; }
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Default config file location
static const char *DEFAULT_CONFIG_PATH = "/etc/fancontrol.conf";

// These defaults can be overridden via config file or CLI
static bool debug = false; // Turn on/off logging
static int interval = 10;  // How often we poll for temperatures

// Fan curve temperatures (in Celsius)
static int temp_low = 35;      // Below this: fans at minimum speed
static int temp_high = 50;     // Above this: fans at maximum speed
// Between temp_low and temp_high: linear interpolation

// Fan speed limits (PWM 0-255)
static int fan_min = 80;       // ~30% - minimum fan speed (never go below this)
static int fan_max = 255;      // 100% - maximum fan speed
static int fan_start = 100;    // ~40% - fan speed at temp_low
const static uint8_t port = 0x2e;
static uint16_t ecbar = 0x00;
static std::string graphite_server; // empty = disabled
static int graphite_port = 0;
static int cputemp_max_values = 10; // Number of values for rolling average of cpu temperature
static bool auto_detect_drives = true; // Auto-detect drives by default
static bool include_nvme = true; // Include NVMe drives in auto-detection
static bool include_hdd = true;  // Include HDD/SSD drives in auto-detection
static int cpu_temp_offset = 20; // CPU temperature offset compared to drives
static bool respect_standby = true; // Don't wake sleeping drives for temperature check

// Drive structure to hold drive info
struct DriveInfo {
    std::string name;
    std::string type; // "hdd", "ssd", "nvme"
    std::string path;
};

void iowrite(uint8_t reg, uint8_t val)
{
  outb(reg, port);
  outb(val, port + 1);
}

uint8_t ioread(uint8_t reg)
{
  outb(reg, port);
  return inb(port + 1);
}

void ecwrite(uint8_t reg, uint8_t val)
{
  outb(reg, ecbar + 5);
  outb(val, ecbar + 6);
}

uint8_t ecread(uint8_t reg)
{
  outb(reg, ecbar + 5);
  return inb(ecbar + 6);
}

// Trim whitespace from string
std::string trim(const std::string &str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// Parse integer, warn and keep fallback on garbage instead of crashing
int parse_int(const std::string &value, const char *key, int fallback)
{
    try {
        return std::stoi(value);
    } catch (...) {
        printf("Warning: invalid value '%s' for %s, keeping %d\n", value.c_str(), key, fallback);
        return fallback;
    }
}

// Drive names get interpolated into shell commands (smartctl/nvme via popen),
// so restrict them to alphanumerics to rule out injection
bool is_valid_drive_name(const std::string &name)
{
    if (name.empty()) return false;
    for (char c : name) {
        if (!isalnum((unsigned char)c)) return false;
    }
    return true;
}

// Check if a file exists
bool file_exists(const char *path)
{
    struct stat buffer;
    return (stat(path, &buffer) == 0);
}

// Check if a block device is a rotational drive (HDD) or not (SSD/NVMe)
bool is_rotational(const std::string &device)
{
    std::string path = "/sys/block/" + device + "/queue/rotational";
    std::ifstream file(path);
    if (file.is_open()) {
        int val;
        file >> val;
        return val == 1;
    }
    return false;
}

// Detect if device is NVMe
bool is_nvme(const std::string &device)
{
    return device.find("nvme") == 0;
}

// Detect available drives in the system
std::vector<DriveInfo> detect_drives()
{
    std::vector<DriveInfo> drives;
    DIR *dir = opendir("/sys/block");
    
    if (dir == nullptr) {
        if (debug) printf("Warning: Could not open /sys/block for drive detection\n");
        return drives;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        
        // Skip . and ..
        if (name == "." || name == "..") continue;
        
        // Skip loop, ram, dm devices
        if (name.find("loop") == 0 || name.find("ram") == 0 || 
            name.find("dm-") == 0 || name.find("sr") == 0 ||
            name.find("fd") == 0) continue;
        
        // Check if this is a physical device by checking for a device symlink
        std::string device_path = "/sys/block/" + name + "/device";
        if (!file_exists(device_path.c_str())) continue;
        
        DriveInfo drive;
        drive.name = name;
        drive.path = "/dev/" + name;
        
        if (is_nvme(name)) {
            drive.type = "nvme";
            if (include_nvme) {
                drives.push_back(drive);
                if (debug) printf("Auto-detected NVMe drive: %s\n", name.c_str());
            }
        } else if (name.find("sd") == 0) {
            // SATA/SAS drive
            drive.type = is_rotational(name) ? "hdd" : "ssd";
            if (include_hdd) {
                drives.push_back(drive);
                if (debug) printf("Auto-detected %s drive: %s\n", drive.type.c_str(), name.c_str());
            }
        }
    }
    
    closedir(dir);
    
    // Sort drives by name for consistent ordering
    std::sort(drives.begin(), drives.end(), [](const DriveInfo &a, const DriveInfo &b) {
        return a.name < b.name;
    });
    
    return drives;
}

// Parse config file
bool parse_config_file(const char *config_path, std::string &drive_list_out)
{
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    std::string current_section;

    while (std::getline(file, line)) {
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        // Check for section header
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        // Parse key=value pairs
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        // Remove quotes from value if present
        if (value.length() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.length() - 2);
        }

        // General settings
        if (key == "debug") {
            debug = (value == "1" || value == "true" || value == "yes");
        } else if (key == "interval") {
            interval = parse_int(value, "interval", interval);
        } else if (key == "cpu_avg" || key == "cpu_avg_samples") {
            cputemp_max_values = parse_int(value, "cpu_avg_samples", cputemp_max_values);
        } else if (key == "cpu_temp_offset") {
            cpu_temp_offset = parse_int(value, "cpu_temp_offset", cpu_temp_offset);
        } else if (key == "graphite_server") {
            size_t colon_pos = value.find(':');
            if (colon_pos != std::string::npos) {
                graphite_server = value.substr(0, colon_pos);
                graphite_port = parse_int(value.substr(colon_pos + 1), "graphite_port", 0);
            } else {
                printf("Warning: graphite_server must be <ip:port>, ignoring '%s'\n", value.c_str());
            }
        }
        // Fan curve settings
        else if (key == "temp_low") {
            temp_low = parse_int(value, "temp_low", temp_low);
        } else if (key == "temp_high") {
            temp_high = parse_int(value, "temp_high", temp_high);
        } else if (key == "fan_min") {
            fan_min = parse_int(value, "fan_min", fan_min);
        } else if (key == "fan_max") {
            fan_max = parse_int(value, "fan_max", fan_max);
        } else if (key == "fan_start") {
            fan_start = parse_int(value, "fan_start", fan_start);
        }
        // Drive settings
        else if (key == "drive_list" || key == "drives") {
            drive_list_out = value;
            auto_detect_drives = false; // Disable auto-detect if drives are manually specified
        } else if (key == "auto_detect" || key == "auto_detect_drives") {
            auto_detect_drives = (value == "1" || value == "true" || value == "yes");
        } else if (key == "include_nvme") {
            include_nvme = (value == "1" || value == "true" || value == "yes");
        } else if (key == "include_hdd" || key == "include_sata") {
            include_hdd = (value == "1" || value == "true" || value == "yes");
        } else if (key == "respect_standby") {
            respect_standby = (value == "1" || value == "true" || value == "yes");
        }
    }

    if (debug) printf("Read config file: %s\n", config_path);
    return true;
}

std::vector<std::string> split_drive_names(const std::string &drive_list)
{
    std::vector<std::string> drives;
    std::stringstream ss(drive_list);
    std::string item;

    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            drives.push_back(item);
        }
    }

    return drives;
}

void print_usage() {
    printf("Usage:\n"
           "\n"
           " fancontrol [--config=<path>] [--auto_detect] [options]\n"
           "\n"
           "Configuration:\n"
           "  --config=<path>       Path to config file (default: /etc/fancontrol.conf)\n"
           "  --generate-config     Generate a sample config file and exit\n"
           "\n"
           "Drive Options:\n"
           "  --drive_list=<list>   Comma-separated list of drive names e.g. 'sda,nvme0n1'\n"
           "  --auto_detect         Auto-detect drives (default)\n"
           "  --no_nvme             Exclude NVMe drives from auto-detection\n"
           "  --no_hdd              Exclude HDD/SSD drives from auto-detection\n"
           "\n"
           "Fan Curve (simple, recommended):\n"
           "  --temp_low=<value>    Temperature for minimum fan speed (default: 35°C)\n"
           "  --temp_high=<value>   Temperature for maximum fan speed (default: 50°C)\n"
           "  --fan_min=<value>     Minimum PWM, fans never go below (default: 80 ~30%%)\n"
           "  --fan_start=<value>   Fan PWM at temp_low (default: 100 ~40%%)\n"
           "  --fan_max=<value>     Maximum PWM at temp_high (default: 255 100%%)\n"
           "\n"
           "Other:\n"
           "  --debug=<0|1>         Enable debug logging (default: 0)\n"
           "  --interval=<value>    Polling interval in seconds (default: 10)\n"
           "  --cpu_temp_offset=<value>  CPU temp offset vs drives (default: 20°C)\n"
           "  --graphite_server=<ip:port>  Graphite server for metrics\n"
           "\n"
           "Fan curve example (with defaults):\n"
           "  Below 35°C  -> 30%% (quiet)\n"
           "  At 35°C     -> 40%% (fan_start)\n"
           "  At 42°C     -> 70%% (interpolated)\n"
           "  At 50°C+    -> 100%% (full speed)\n"
           "\n"
           "Config file settings are overridden by command line arguments.\n");
}

void generate_sample_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("Error: Could not create config file at %s\n", path);
        return;
    }
    
    fprintf(f, "# TerraMaster Fan Control Configuration\n");
    fprintf(f, "# Generated sample configuration file\n");
    fprintf(f, "\n");
    fprintf(f, "[general]\n");
    fprintf(f, "# Enable debug logging (true/false)\n");
    fprintf(f, "debug = false\n");
    fprintf(f, "\n");
    fprintf(f, "# Polling interval in seconds\n");
    fprintf(f, "interval = 10\n");
    fprintf(f, "\n");
    fprintf(f, "[drives]\n");
    fprintf(f, "# Auto-detect drives (recommended)\n");
    fprintf(f, "auto_detect = true\n");
    fprintf(f, "\n");
    fprintf(f, "# Include NVMe drives in monitoring\n");
    fprintf(f, "include_nvme = true\n");
    fprintf(f, "\n");
    fprintf(f, "# Include HDD/SSD (SATA/SAS) drives in monitoring\n");
    fprintf(f, "include_hdd = true\n");
    fprintf(f, "\n");
    fprintf(f, "# Don't wake sleeping SATA drives for temperature checks\n");
    fprintf(f, "# Sleeping drives are skipped (they are cool anyway)\n");
    fprintf(f, "respect_standby = true\n");
    fprintf(f, "\n");
    fprintf(f, "# Manual drive list (disables auto_detect if specified)\n");
    fprintf(f, "# drive_list = sda,sdb,sdc,sdd,nvme0n1\n");
    fprintf(f, "\n");
    fprintf(f, "[fan_curve]\n");
    fprintf(f, "# Simple fan curve: linear interpolation between temp_low and temp_high\n");
    fprintf(f, "#\n");
    fprintf(f, "# How it works:\n");
    fprintf(f, "#   - Below temp_low:  fans run at fan_min (quiet)\n");
    fprintf(f, "#   - At temp_low:     fans run at fan_start\n");
    fprintf(f, "#   - At temp_high:    fans run at fan_max (full speed)\n");
    fprintf(f, "#   - Between:         linear interpolation\n");
    fprintf(f, "#\n");
    fprintf(f, "# Example with defaults (temp_low=35, temp_high=50):\n");
    fprintf(f, "#   30°C -> 30%% (fan_min)\n");
    fprintf(f, "#   35°C -> 40%% (fan_start)\n");
    fprintf(f, "#   42°C -> 70%%\n");
    fprintf(f, "#   50°C -> 100%% (fan_max)\n");
    fprintf(f, "#\n");
    fprintf(f, "\n");
    fprintf(f, "# Temperature thresholds (Celsius)\n");
    fprintf(f, "temp_low = 35\n");
    fprintf(f, "temp_high = 50\n");
    fprintf(f, "\n");
    fprintf(f, "# Fan speed limits (PWM: 0-255)\n");
    fprintf(f, "# fan_min: absolute minimum, fans never go below this (~30%%)\n");
    fprintf(f, "fan_min = 80\n");
    fprintf(f, "\n");
    fprintf(f, "# fan_start: speed at temp_low (~40%%)\n");
    fprintf(f, "fan_start = 100\n");
    fprintf(f, "\n");
    fprintf(f, "# fan_max: maximum speed at temp_high (100%%)\n");
    fprintf(f, "fan_max = 255\n");
    fprintf(f, "\n");
    fprintf(f, "[cpu]\n");
    fprintf(f, "# CPU temperature offset compared to drives\n");
    fprintf(f, "# (CPU is allowed to be this many degrees hotter)\n");
    fprintf(f, "cpu_temp_offset = 20\n");
    fprintf(f, "\n");
    fprintf(f, "# Number of CPU temperature samples for rolling average\n");
    fprintf(f, "cpu_avg_samples = 10\n");
    fprintf(f, "\n");
    fprintf(f, "[graphite]\n");
    fprintf(f, "# Graphite server for metrics (optional)\n");
    fprintf(f, "# Format: ip:port\n");
    fprintf(f, "# graphite_server = 192.168.1.100:2003\n");
    
    fclose(f);
    printf("Sample configuration file generated at: %s\n", path);
}

// Connect to the configured Graphite server. Returns fd or -1.
int graphite_connect()
{
    struct sockaddr_in servaddr;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("Error: Could not create Graphite socket\n");
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(graphite_port);

    if (inet_pton(AF_INET, graphite_server.c_str(), &servaddr.sin_addr) <= 0) {
        printf("Invalid Graphite address: %s\n", graphite_server.c_str());
        close(sockfd);
        return -1;
    }
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        if (debug) printf("Graphite connection to %s:%d failed\n", graphite_server.c_str(), graphite_port);
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Send a metric. MSG_NOSIGNAL so a dead peer can't SIGPIPE-kill the daemon;
// on failure the socket is closed and reconnected on the next poll cycle.
void send_to_graphite(int &sockfd, const char *message)
{
    if (sockfd < 0) return;
    if (send(sockfd, message, strlen(message), MSG_NOSIGNAL) < 0) {
        if (debug) printf("Graphite send failed, will reconnect\n");
        close(sockfd);
        sockfd = -1;
    }
}

// Simple fan curve calculation
// Returns PWM value based on temperature using linear interpolation
//
// Fan Curve Visualization:
//
//  fan_max |                  ********
//          |              ****
//          |          ****
// fan_start|      ****
//          |******
//  fan_min |---------------------------
//          temp_low      temp_high
//
int calculate_fan_speed(int temperature) {
    int pwm;
    if (temperature <= temp_low) {
        // Below temp_low: run at minimum speed
        pwm = fan_min;
    } else if (temperature >= temp_high) {
        // Above temp_high: run at maximum speed
        pwm = fan_max;
    } else {
        // Linear interpolation between temp_low and temp_high
        // Map temperature range to fan speed range
        double temp_range = temp_high - temp_low;
        double fan_range = fan_max - fan_start;
        double temp_ratio = (temperature - temp_low) / temp_range;
        pwm = fan_start + (int)(temp_ratio * fan_range);

        // Ensure we never go below fan_min
        if (pwm < fan_min) pwm = fan_min;
    }

    // Clamp to valid PWM range — the EC register is 8-bit, anything else wraps
    if (pwm < 0) pwm = 0;
    if (pwm > 255) pwm = 255;

    return pwm;
}

// Get temperature for NVMe drive - prefer hwmon (no process spawn) over nvme-cli
int get_nvme_temperature(const char *device)
{
    char path[576];
    int temp = -1; // -1 = no reading yet (0 is a valid temperature)

    // First try: hwmon (fastest, no process spawn, reports in millidegrees Celsius)
    // This is the preferred method as it doesn't spawn any processes
    char hwmon_path[256];
    snprintf(hwmon_path, sizeof(hwmon_path), "/sys/block/%s/device/hwmon", device);
    DIR *hwmon_dir = opendir(hwmon_path);
    if (hwmon_dir) {
        struct dirent *entry;
        while ((entry = readdir(hwmon_dir)) != nullptr) {
            if (strncmp(entry->d_name, "hwmon", 5) == 0) {
                snprintf(path, sizeof(path), "%s/%s/temp1_input", hwmon_path, entry->d_name);
                FILE *tf = fopen(path, "r");
                if (tf) {
                    int millidegrees;
                    if (fscanf(tf, "%d", &millidegrees) == 1) {
                        temp = millidegrees / 1000;
                    }
                    fclose(tf);
                }
                break;
            }
        }
        closedir(hwmon_dir);
    }

    // Fallback: nvme-cli (spawns process, but reliable)
    if (temp < 0) {
        char cmd[256];
        char tempstring[128];

        // For nvmeXnY, query nvmeX (the controller, not the namespace):
        // strip everything from the 'n' that follows the controller digits
        std::string controller(device);
        if (controller.compare(0, 4, "nvme") == 0) {
            size_t n_pos = controller.find('n', 4);
            if (n_pos != std::string::npos) {
                controller = controller.substr(0, n_pos);
            }
        }

        // Use LC_ALL=C to ensure Celsius output
        snprintf(cmd, sizeof(cmd), "LC_ALL=C nvme smart-log /dev/%s 2>/dev/null | grep -E '^temperature' | head -1", controller.c_str());
        FILE *pipe = popen(cmd, "r");
        if (pipe) {
            if (fgets(tempstring, sizeof(tempstring), pipe)) {
                char *colon = strchr(tempstring, ':');
                if (colon) {
                    temp = atoi(colon + 1);
                }
            }
            pclose(pipe);
        }
    }

    return temp < 0 ? 0 : temp;
}

// Get CPU temperature directly from sysfs (avoids spawning sensors process)
// Returns temperature in Celsius, or 0 if not available
int get_cpu_temperature_sysfs()
{
    // Try common thermal zone paths
    const char *thermal_paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",  // Most common
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/devices/platform/coretemp.0/hwmon/hwmon*/temp1_input",
        NULL
    };
    
    // First, try thermal_zone paths directly
    for (int i = 0; thermal_paths[i] != NULL; i++) {
        FILE *f = fopen(thermal_paths[i], "r");
        if (f) {
            int millidegrees;
            if (fscanf(f, "%d", &millidegrees) == 1) {
                fclose(f);
                return millidegrees / 1000;
            }
            fclose(f);
        }
    }
    
    // Try to find coretemp hwmon
    DIR *hwmon_dir = opendir("/sys/class/hwmon");
    if (hwmon_dir) {
        struct dirent *entry;
        while ((entry = readdir(hwmon_dir)) != nullptr) {
            if (strncmp(entry->d_name, "hwmon", 5) == 0) {
                char name_path[320];
                snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", entry->d_name);
                FILE *nf = fopen(name_path, "r");
                if (nf) {
                    char name[64];
                    if (fgets(name, sizeof(name), nf)) {
                        // Look for coretemp or k10temp (AMD)
                        if (strstr(name, "coretemp") || strstr(name, "k10temp")) {
                            fclose(nf);
                            // Read temp1_input (Package temp)
                            char temp_path[320];
                            snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/%s/temp1_input", entry->d_name);
                            FILE *tf = fopen(temp_path, "r");
                            if (tf) {
                                int millidegrees;
                                if (fscanf(tf, "%d", &millidegrees) == 1) {
                                    fclose(tf);
                                    closedir(hwmon_dir);
                                    return millidegrees / 1000;
                                }
                                fclose(tf);
                            }
                        }
                    }
                    fclose(nf);
                }
            }
        }
        closedir(hwmon_dir);
    }
    
    return 0; // Not found
}

// Get temperature for SATA/SAS drive
// Returns 0 if drive is in standby (to avoid waking it)
int get_sata_temperature(const char *device)
{
    char cmd[256];
    char output[256];
    int temp = 0;

    // With respect_standby, pass -n standby so smartctl reports
    // "Device is in STANDBY mode" and exits instead of waking the drive
    const char *standby_flag = respect_standby ? "-n standby" : "";

    snprintf(cmd, sizeof(cmd), "smartctl %s -A -d sat /dev/%s 2>&1", standby_flag, device);
    FILE *pipe = popen(cmd, "r");
    if (pipe) {
        bool drive_sleeping = false;
        while (fgets(output, sizeof(output), pipe)) {
            // Check for standby message
            if (strstr(output, "STANDBY") || strstr(output, "standby")) {
                drive_sleeping = true;
                if (debug) printf("Drive %s is sleeping, skipping\n", device);
            }
            // Parse temperature if present and drive is awake
            if (!drive_sleeping && (strstr(output, "Temperature_Celsius") || strstr(output, "Airflow_Temperature"))) {
                char *token = strtok(output, " \t");
                int field = 0;
                while (token && field < 9) {
                    token = strtok(NULL, " \t");
                    field++;
                }
                if (token) {
                    temp = atoi(token);
                }
            }
        }
        pclose(pipe);

        if (drive_sleeping) {
            return 0; // Ignore sleeping drives
        }
    }

    // If that failed, try without -d sat
    if (temp == 0) {
        snprintf(cmd, sizeof(cmd), "smartctl %s -A /dev/%s 2>&1", standby_flag, device);
        pipe = popen(cmd, "r");
        if (pipe) {
            bool drive_sleeping = false;
            while (fgets(output, sizeof(output), pipe)) {
                if (strstr(output, "STANDBY") || strstr(output, "standby")) {
                    drive_sleeping = true;
                }
                if (!drive_sleeping && (strstr(output, "Temperature_Celsius") || strstr(output, "Airflow_Temperature"))) {
                    char *token = strtok(output, " \t");
                    int field = 0;
                    while (token && field < 9) {
                        token = strtok(NULL, " \t");
                        field++;
                    }
                    if (token) {
                        temp = atoi(token);
                    }
                }
            }
            pclose(pipe);

            if (drive_sleeping) {
                return 0;
            }
        }
    }

    return temp;
}

// Sanity-check settings after config + CLI parsing. Returns false on fatal problems.
bool validate_settings()
{
    bool ok = true;

    if (interval < 1) {
        printf("Error: interval must be >= 1 second (got %d)\n", interval);
        ok = false;
    }
    if (cputemp_max_values < 1) {
        printf("Error: cpu_avg_samples must be >= 1 (got %d)\n", cputemp_max_values);
        ok = false;
    }
    if (temp_high <= temp_low) {
        printf("Error: temp_high (%d) must be greater than temp_low (%d)\n", temp_high, temp_low);
        ok = false;
    }
    if (fan_min < 0 || fan_min > 255 ||
        fan_start < 0 || fan_start > 255 ||
        fan_max < 0 || fan_max > 255) {
        printf("Error: fan_min/fan_start/fan_max must be in range 0-255 (got %d/%d/%d)\n",
               fan_min, fan_start, fan_max);
        ok = false;
    }
    if (ok && fan_max < fan_start) {
        printf("Error: fan_max (%d) must be >= fan_start (%d)\n", fan_max, fan_start);
        ok = false;
    }
    if (!graphite_server.empty() && (graphite_port < 1 || graphite_port > 65535)) {
        printf("Error: graphite port must be in range 1-65535 (got %d)\n", graphite_port);
        ok = false;
    }

    return ok;
}

#ifndef FANCONTROL_NO_MAIN
int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG_PATH;
    std::string drive_list;
    bool generate_config = false;
    const char *generate_config_path = NULL;

    // First pass: check for config path and generate-config flag
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--generate-config") == 0) {
            generate_config = true;
            generate_config_path = DEFAULT_CONFIG_PATH;
        } else if (strncmp(argv[i], "--generate-config=", 18) == 0) {
            generate_config = true;
            generate_config_path = argv[i] + 18;
        }
    }

    // Generate config and exit if requested
    if (generate_config) {
        generate_sample_config(generate_config_path);
        return 0;
    }

    // Load config file if it exists
    if (file_exists(config_path)) {
        parse_config_file(config_path, drive_list);
        if (debug) printf("Loaded configuration from %s\n", config_path);
    }

    // Second pass: parse CLI arguments (override config file settings)
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            // Already processed
        } else if (strncmp(argv[i], "--drive_list=", 13) == 0) {
            drive_list = argv[i] + 13;
            auto_detect_drives = false;
        } else if (strcmp(argv[i], "--auto_detect") == 0) {
            auto_detect_drives = true;
            drive_list.clear();
        } else if (strcmp(argv[i], "--no_nvme") == 0) {
            include_nvme = false;
        } else if (strcmp(argv[i], "--no_hdd") == 0) {
            include_hdd = false;
        } else if (strncmp(argv[i], "--debug=", 8) == 0) {
            debug = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--interval=", 11) == 0) {
            interval = parse_int(argv[i] + 11, "interval", interval);
        // Fan curve parameters
        } else if (strncmp(argv[i], "--temp_low=", 11) == 0) {
            temp_low = parse_int(argv[i] + 11, "temp_low", temp_low);
        } else if (strncmp(argv[i], "--temp_high=", 12) == 0) {
            temp_high = parse_int(argv[i] + 12, "temp_high", temp_high);
        } else if (strncmp(argv[i], "--fan_min=", 10) == 0) {
            fan_min = parse_int(argv[i] + 10, "fan_min", fan_min);
        } else if (strncmp(argv[i], "--fan_start=", 12) == 0) {
            fan_start = parse_int(argv[i] + 12, "fan_start", fan_start);
        } else if (strncmp(argv[i], "--fan_max=", 10) == 0) {
            fan_max = parse_int(argv[i] + 10, "fan_max", fan_max);
        } else if (strncmp(argv[i], "--cpu_avg=", 10) == 0) {
            cputemp_max_values = parse_int(argv[i] + 10, "cpu_avg", cputemp_max_values);
        } else if (strncmp(argv[i], "--cpu_temp_offset=", 18) == 0) {
            cpu_temp_offset = parse_int(argv[i] + 18, "cpu_temp_offset", cpu_temp_offset);
        } else if (strncmp(argv[i], "--graphite_server=", 18) == 0) {
            std::string server_info = argv[i] + 18;
            size_t colon_pos = server_info.find(':');
            if (colon_pos != std::string::npos) {
                graphite_server = server_info.substr(0, colon_pos);
                graphite_port = parse_int(server_info.substr(colon_pos + 1), "graphite_port", 0);
            } else {
                printf("Invalid Graphite server format. Expected <ip:port>\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strncmp(argv[i], "--generate-config", 17) == 0) {
            // Already processed
        } else {
            printf("Unknown parameter: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (!validate_settings()) {
        return 1;
    }

    // Build drive list
    std::vector<std::string> drives;

    if (auto_detect_drives || drive_list.empty()) {
        // Auto-detect drives
        std::vector<DriveInfo> detected_drives = detect_drives();

        if (detected_drives.empty()) {
            printf("Error: No drives detected. Please specify drives manually with --drive_list\n");
            return 1;
        }

        printf("Auto-detected %zu drive(s):\n", detected_drives.size());
        for (const auto &drive : detected_drives) {
            drives.push_back(drive.name);
            printf("  - %s (%s)\n", drive.name.c_str(), drive.type.c_str());
        }
    } else {
        drives = split_drive_names(drive_list);

        if (drives.empty()) {
            printf("Error: No drives specified.\n");
            return 1;
        }

        printf("Monitoring %zu manually specified drive(s):\n", drives.size());
        for (const auto &drive : drives) {
            printf("  - %s\n", drive.c_str());
        }
    }

    // Drive names end up in shell commands — reject anything suspicious
    for (const auto &drive : drives) {
        if (!is_valid_drive_name(drive)) {
            printf("Error: invalid drive name '%s' (only alphanumeric names allowed)\n", drive.c_str());
            return 1;
        }
    }
    const int count = (int)drives.size();

    // Print configuration summary
    if (debug) {
        printf("\nConfiguration:\n");
        printf("  Interval: %ds\n", interval);
        printf("  CPU Temp Offset: %d°C\n", cpu_temp_offset);
        printf("  Fan curve:\n");
        printf("    Below %d°C -> %d PWM (%.0f%%) [minimum]\n", temp_low, fan_min, (fan_min/255.0)*100);
        printf("    At %d°C    -> %d PWM (%.0f%%) [start ramping]\n", temp_low, fan_start, (fan_start/255.0)*100);
        printf("    At %d°C    -> %d PWM (%.0f%%) [full speed]\n", temp_high, fan_max, (fan_max/255.0)*100);
        if (!graphite_server.empty()) {
            printf("  Graphite: %s:%d\n", graphite_server.c_str(), graphite_port);
        }
        printf("\n");
    }

    // Obtain access to IO ports
    if (iopl(3) != 0) {
        printf("Error: Failed to get IO port access. Are you running as root?\n");
        return 1;
    }

    // Initialize the IT8613E
    outb(0x87, port);
    outb(0x01, port);
    outb(0x55, port);
    outb(0x55, port);

    // Sanity checks commented out so that it works for both chips.
    // Sanity check that this is the IT8772E
    //assert(ioread(0x20) == 0x87);
    //assert(ioread(0x21) == 0x72);

    // Sanity check that this is the IT8613E
    //assert(ioread(0x20) == 0x86);
    //assert(ioread(0x21) == 0x13);

    // Set LDN = 4 to access environment registers
    iowrite(0x07, 0x04);

    // Activate environment controller (EC)
    iowrite(0x30, 0x01);

    // Read EC bar
    ecbar = (ioread(0x60) << 8) + ioread(0x61);

    // Initialize the PWM value
    uint8_t pwm = fan_min;
    ecwrite(0x6b, pwm);
    ecwrite(0x73, pwm);

    // Set software operation
    ecwrite(0x16, 0x00);
    ecwrite(0x17, 0x00);

    int maxtemp = 0;

    std::vector<int> cputemp_values(cputemp_max_values, 0);  // Store last N values
    int cputemp_index = 0;  // Circular index
    int cputemp_count = 0;  // Number of values stored
    int cputemp_sum = 0;    // Sum of stored values
    int cpu_avg_temp = 0; // Average CPU temperature

    // Setup graphite socket (reconnected on failure inside the loop)
    int graphite_sockfd = -1;

    printf("Fan control started. Monitoring %d drives...\n", count);

    while (true)
    {
        maxtemp = 0;

        // (Re)connect Graphite if configured and not connected
        if (!graphite_server.empty() && graphite_sockfd < 0) {
            graphite_sockfd = graphite_connect();
        }

        // Get temperature for each drive
        for (int i = 0; i < count; ++i)
        {
            int temp = 0;
            bool is_nvme_drive = is_nvme(drives[i]);

            if (is_nvme_drive) {
                temp = get_nvme_temperature(drives[i].c_str());
            } else {
                temp = get_sata_temperature(drives[i].c_str());
            }

            if (temp > maxtemp) maxtemp = temp;

            if (debug) printf("Drive: /dev/%s (%s) temperature: %d°C\n",
                            drives[i].c_str(),
                            is_nvme_drive ? "NVMe" : "SATA",
                            temp);

            // Send disk temperature to Graphite
            if (graphite_sockfd >= 0) {
                char message[256];
                snprintf(message, sizeof(message), "fancontrol.%s %d %ld\n", drives[i].c_str(), temp, time(NULL));
                send_to_graphite(graphite_sockfd, message);
            }
        }

        // Get CPU temperature (prefer sysfs, fallback to sensors command)
        int cputemp = get_cpu_temperature_sysfs();
        
        // Fallback to sensors command if sysfs failed
        if (cputemp == 0) {
            FILE *cpupipe = popen("sensors 2>/dev/null | grep -i 'Package id' | awk -F'[+.°]' '{print $2}'", "r");
            if (cpupipe) {
                char cputempstring[10];
                if (fgets(cputempstring, sizeof(cputempstring), cpupipe)) {
                    cputemp = atoi(cputempstring);
                }
                pclose(cpupipe);
            }
        }
        
        if (cputemp > 0) {
            // Rolling average logic
            if (cputemp_count < cputemp_max_values) {
                // If not full, just add value
                cputemp_sum += cputemp;
                cputemp_values[cputemp_count] = cputemp;
                cputemp_count++;
            } else {
                // If full, replace oldest value
                cputemp_sum = cputemp_sum - cputemp_values[cputemp_index] + cputemp;
                cputemp_values[cputemp_index] = cputemp;
            }

            // Update circular index
            cputemp_index = (cputemp_index + 1) % cputemp_max_values;

            // Compute rolling average
            cpu_avg_temp = cputemp_sum / cputemp_count;

            // Allow CPU to be cpu_temp_offset degrees higher than drives
            if (cpu_avg_temp - cpu_temp_offset > maxtemp) {
                maxtemp = cpu_avg_temp - cpu_temp_offset;
            }

            if (debug) printf("CPU Temperature: %d°C | Rolling Avg (last %d): %d°C\n", 
                            cputemp, cputemp_count, cpu_avg_temp);
        }

        if (debug) printf("Max Temperature: %d°C\n", maxtemp);

        if (graphite_sockfd >= 0) {
            char message[256];

            snprintf(message, sizeof(message), "fancontrol.maxtemp %d %ld\n", maxtemp, time(NULL));
            send_to_graphite(graphite_sockfd, message);
        }

        // Calculate fan speed based on temperature
        pwm = calculate_fan_speed(maxtemp);
        
        if (debug) {
            printf("Fan curve: temp=%d°C -> pwm=%d (%.0f%%)\n",
                   maxtemp, pwm, (pwm / 255.0) * 100);
        }

        // Write new PWM
        ecwrite(0x6b, pwm);
        ecwrite(0x73, pwm);

        // Send PWM value to Graphite if configured
        if (graphite_sockfd >= 0) {
            char message[256];

            // Send PWM value
            snprintf(message, sizeof(message), "fancontrol.pwm %d %ld\n", pwm, time(NULL));
            send_to_graphite(graphite_sockfd, message);

            // Send CPU average temperature
            snprintf(message, sizeof(message), "fancontrol.cpu_avg_temp %d %ld\n", cpu_avg_temp, time(NULL));
            send_to_graphite(graphite_sockfd, message);
        }

        // Sleep at end of loop
        sleep(interval);
    }
}
#endif // FANCONTROL_NO_MAIN
