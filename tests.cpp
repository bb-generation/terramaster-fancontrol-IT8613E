// Unit tests for the pure logic in fancontrol.cpp.
// Build & run: make test

#define FANCONTROL_NO_MAIN
#include "fancontrol.cpp"

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_trim()
{
    CHECK(trim("  hello  ") == "hello");
    CHECK(trim("\t x \r\n") == "x");
    CHECK(trim("") == "");
    CHECK(trim("   ") == "");
    CHECK(trim("nospace") == "nospace");
}

static void test_split_drive_names()
{
    auto d = split_drive_names("sda,sdb,nvme0n1");
    CHECK(d.size() == 3);
    CHECK(d[0] == "sda" && d[1] == "sdb" && d[2] == "nvme0n1");

    // Trailing comma must not produce a phantom entry (was a crash before)
    CHECK(split_drive_names("sda,sdb,").size() == 2);
    // Empty segments skipped
    CHECK(split_drive_names("sda,,sdb").size() == 2);
    // Whitespace trimmed
    d = split_drive_names(" sda , sdb ");
    CHECK(d.size() == 2 && d[0] == "sda" && d[1] == "sdb");
    // Empty input
    CHECK(split_drive_names("").empty());
    CHECK(split_drive_names(",,,").empty());
}

static void test_is_valid_drive_name()
{
    CHECK(is_valid_drive_name("sda"));
    CHECK(is_valid_drive_name("nvme0n1"));
    CHECK(!is_valid_drive_name(""));
    CHECK(!is_valid_drive_name("sda; reboot"));
    CHECK(!is_valid_drive_name("../sda"));
    CHECK(!is_valid_drive_name("sd a"));
    CHECK(!is_valid_drive_name("$(id)"));
}

static void test_parse_int()
{
    CHECK(parse_int("42", "test", 0) == 42);
    CHECK(parse_int("-5", "test", 0) == -5);
    // Garbage falls back instead of throwing
    CHECK(parse_int("abc", "test", 7) == 7);
    CHECK(parse_int("", "test", 9) == 9);
}

static void test_parse_temp_source()
{
    TempSource src = TEMP_SOURCE_SMART;
    CHECK(parse_temp_source("hwmon", src) && src == TEMP_SOURCE_HWMON);
    CHECK(parse_temp_source("smart", src) && src == TEMP_SOURCE_SMART);

    // Garbage keeps the previous value
    src = TEMP_SOURCE_HWMON;
    CHECK(!parse_temp_source("drivetemp", src) && src == TEMP_SOURCE_HWMON);
    CHECK(!parse_temp_source("", src) && src == TEMP_SOURCE_HWMON);
}

static void test_calculate_fan_speed()
{
    temp_low = 35; temp_high = 50;
    fan_min = 80; fan_start = 100; fan_max = 255;

    CHECK(calculate_fan_speed(0) == 80);     // far below -> fan_min
    CHECK(calculate_fan_speed(35) == 80);    // at temp_low -> fan_min (<=)
    CHECK(calculate_fan_speed(50) == 255);   // at temp_high -> fan_max
    CHECK(calculate_fan_speed(90) == 255);   // above -> fan_max

    int mid = calculate_fan_speed(42);       // interpolated, monotonic
    CHECK(mid > 100 && mid < 255);
    CHECK(calculate_fan_speed(43) >= mid);

    // 8-bit clamp: out-of-range settings must not wrap the PWM register
    fan_max = 300;
    CHECK(calculate_fan_speed(90) == 255);
    fan_min = -20;
    CHECK(calculate_fan_speed(0) == 0);
    fan_min = 80; fan_max = 255;
}

static void test_validate_settings()
{
    // Valid defaults
    interval = 10; cputemp_max_values = 10;
    temp_low = 35; temp_high = 50;
    fan_min = 80; fan_start = 100; fan_max = 255;
    graphite_server.clear(); graphite_port = 0;
    CHECK(validate_settings());

    interval = 0;
    CHECK(!validate_settings());
    interval = 10;

    cputemp_max_values = 0; // was a division-by-zero crash before
    CHECK(!validate_settings());
    cputemp_max_values = 10;

    temp_high = temp_low; // was a division-by-zero in the curve before
    CHECK(!validate_settings());
    temp_high = 50;

    fan_max = 300; // was a uint8_t wraparound before
    CHECK(!validate_settings());
    fan_max = 255;

    fan_start = 200; fan_max = 100;
    CHECK(!validate_settings());
    fan_start = 100; fan_max = 255;

    graphite_server = "192.168.1.1"; graphite_port = 0;
    CHECK(!validate_settings());
    graphite_port = 2003;
    CHECK(validate_settings());
    graphite_server.clear(); graphite_port = 0;
}

static void test_parse_config_file()
{
    const char *path = "/tmp/fancontrol_test.conf";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f,
            "# comment\n"
            "[general]\n"
            "interval = 20\n"
            "bogus_number = abc\n"
            "[fan_curve]\n"
            "temp_low = 30\n"
            "temp_high = 55\n"
            "fan_max = notanumber\n"   // must not crash, keeps previous value
            "[drives]\n"
            "drive_list = \"sda, sdb\"\n"
            "temp_source = hwmon\n"
            "[graphite]\n"
            "graphite_server = 10.0.0.1:2003\n");
    fclose(f);

    interval = 10; temp_low = 35; temp_high = 50; fan_max = 255;
    auto_detect_drives = true;
    temp_source = TEMP_SOURCE_SMART;
    std::string drive_list;

    CHECK(parse_config_file(path, drive_list));
    CHECK(interval == 20);
    CHECK(temp_low == 30);
    CHECK(temp_high == 55);
    CHECK(fan_max == 255);            // invalid value fell back, no crash
    CHECK(drive_list == "sda, sdb");  // quotes stripped
    CHECK(!auto_detect_drives);       // manual list disables auto-detect
    CHECK(temp_source == TEMP_SOURCE_HWMON);
    CHECK(graphite_server == "10.0.0.1");
    CHECK(graphite_port == 2003);

    remove(path);
    graphite_server.clear(); graphite_port = 0;
    temp_source = TEMP_SOURCE_SMART;
}

static void test_parse_config_default_temp_source()
{
    // A config without temp_source must keep the smartctl path
    const char *path = "/tmp/fancontrol_test_default.conf";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "[drives]\nauto_detect = true\n");
    fclose(f);

    temp_source = TEMP_SOURCE_SMART;
    std::string drive_list;
    CHECK(parse_config_file(path, drive_list));
    CHECK(temp_source == TEMP_SOURCE_SMART);
    remove(path);
}

static void test_is_nvme()
{
    CHECK(is_nvme("nvme0n1"));
    CHECK(is_nvme("nvme10n2"));
    CHECK(!is_nvme("sda"));
    CHECK(!is_nvme("xnvme"));
}

static void test_nvme_controller_name()
{
    CHECK(nvme_controller_name("nvme0n1") == "nvme0");
    CHECK(nvme_controller_name("nvme10n2") == "nvme10");
    CHECK(nvme_controller_name("nvme0") == "nvme0");
    CHECK(nvme_controller_name("sda") == "sda");
}

// --- Fake sysfs tree for the hwmon lookup ---

static void make_dirs(const std::string &path)
{
    for (size_t pos = 1; (pos = path.find('/', pos)) != std::string::npos; ++pos) {
        mkdir(path.substr(0, pos).c_str(), 0755);
    }
    mkdir(path.c_str(), 0755);
}

static void write_file(const std::string &path, const std::string &content)
{
    FILE *f = fopen(path.c_str(), "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs(content.c_str(), f);
    fclose(f);
}

static void make_link(const std::string &target, const std::string &link)
{
    CHECK(symlink(target.c_str(), link.c_str()) == 0);
}

static void make_hwmon(const std::string &root, const char *hwmon, const char *name,
                       const std::string &device, const char *millidegrees)
{
    std::string dir = root + "/class/hwmon/" + hwmon;
    make_dirs(dir);
    write_file(dir + "/name", std::string(name) + "\n");
    write_file(dir + "/temp1_input", millidegrees);
    make_link(device, dir + "/device");
}

static void test_get_hwmon_drive_temperature()
{
    char tmpl[] = "/tmp/fancontrol_sysfs_XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    const std::string root = tmpl;
    const std::string dev = root + "/devices";

    // SATA: block/sdX/device -> SCSI device; drivetemp hangs off that
    const std::string sda = dev + "/pci/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0";
    const std::string sdb = dev + "/pci/0000:00:17.0/ata2/host1/target1:0:0/1:0:0:0";
    const std::string sdc = dev + "/pci/0000:00:17.0/ata3/host2/target2:0:0/2:0:0:0";
    // NVMe: controllers under their PCI functions
    const std::string pci0 = dev + "/pci/0000:01:00.0", ctrl0 = pci0 + "/nvme/nvme0";
    const std::string pci1 = dev + "/pci/0000:02:00.0", ctrl1 = pci1 + "/nvme/nvme1";
    const std::string pci2 = dev + "/pci/0000:03:00.0", ctrl2 = pci2 + "/nvme/nvme2";
    const std::string subsys1 = dev + "/virtual/nvme-subsystem/nvme-subsys1";
    for (const auto &d : {sda, sdb, sdc, ctrl0, ctrl1, ctrl2, subsys1}) make_dirs(d);
    make_link(pci0, ctrl0 + "/device");
    make_link(pci1, ctrl1 + "/device");
    make_link(pci2, ctrl2 + "/device");

    make_dirs(root + "/class/nvme");
    make_link(ctrl0, root + "/class/nvme/nvme0");
    make_link(ctrl1, root + "/class/nvme/nvme1");
    make_link(ctrl2, root + "/class/nvme/nvme2");

    for (const char *b : {"sda", "sdb", "sdc", "nvme0n1", "nvme1n1", "nvme2n1"}) {
        make_dirs(root + "/block/" + b);
    }
    make_link(sda, root + "/block/sda/device");
    make_link(sdb, root + "/block/sdb/device");
    make_link(sdc, root + "/block/sdc/device");
    make_link(ctrl0, root + "/block/nvme0n1/device");
    make_link(subsys1, root + "/block/nvme1n1/device"); // native NVMe multipath
    make_link(ctrl2, root + "/block/nvme2n1/device");

    // hwmon numbering deliberately doesn't follow drive order (hwmon1 = sdb)
    make_hwmon(root, "hwmon0", "coretemp", sda, "99000");   // wrong driver, same parent
    make_hwmon(root, "hwmon1", "drivetemp", sdb, "50000");
    make_hwmon(root, "hwmon2", "drivetemp", sda, "44000");
    make_hwmon(root, "hwmon3", "nvme", ctrl0, "38850");
    make_hwmon(root, "hwmon4", "nvme", ctrl1, "41000");
    make_hwmon(root, "hwmon5", "nvme", pci2, "36000");       // older kernels: PCI function
    make_hwmon(root, "hwmon6", "drivetemp", sdc, "");        // read error (e.g. EIO)

    CHECK(get_hwmon_drive_temperature("sda", root) == 44);
    CHECK(get_hwmon_drive_temperature("sdb", root) == 50);
    CHECK(get_hwmon_drive_temperature("nvme0n1", root) == 38);   // millidegrees truncated
    CHECK(get_hwmon_drive_temperature("nvme1n1", root) == 41);
    CHECK(get_hwmon_drive_temperature("nvme2n1", root) == 36);
    CHECK(get_hwmon_drive_temperature("sdc", root) == -1);       // unreadable sensor
    CHECK(get_hwmon_drive_temperature("sdd", root) == -1);       // no such drive
    CHECK(get_hwmon_drive_temperature("sda", root + "/nope") == -1);

    // drivetemp unloaded: SATA sensors gone, NVMe unaffected
    CHECK(system(("rm -rf '" + root + "/class/hwmon/hwmon1' '" + root + "/class/hwmon/hwmon2'").c_str()) == 0);
    CHECK(get_hwmon_drive_temperature("sda", root) == -1);
    CHECK(get_hwmon_drive_temperature("sdb", root) == -1);
    CHECK(get_hwmon_drive_temperature("nvme0n1", root) == 38);

    CHECK(system(("rm -rf '" + root + "'").c_str()) == 0);
}

int main()
{
    // Compiled-in default, checked before any test mutates the globals
    CHECK(temp_source == TEMP_SOURCE_HWMON);

    test_trim();
    test_split_drive_names();
    test_is_valid_drive_name();
    test_parse_int();
    test_parse_temp_source();
    test_calculate_fan_speed();
    test_validate_settings();
    test_parse_config_file();
    test_parse_config_default_temp_source();
    test_is_nvme();
    test_nvme_controller_name();
    test_get_hwmon_drive_temperature();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) FAILED.\n", failures);
    return 1;
}
