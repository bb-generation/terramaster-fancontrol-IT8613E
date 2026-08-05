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
            "[graphite]\n"
            "graphite_server = 10.0.0.1:2003\n");
    fclose(f);

    interval = 10; temp_low = 35; temp_high = 50; fan_max = 255;
    auto_detect_drives = true;
    std::string drive_list;

    CHECK(parse_config_file(path, drive_list));
    CHECK(interval == 20);
    CHECK(temp_low == 30);
    CHECK(temp_high == 55);
    CHECK(fan_max == 255);            // invalid value fell back, no crash
    CHECK(drive_list == "sda, sdb");  // quotes stripped
    CHECK(!auto_detect_drives);       // manual list disables auto-detect
    CHECK(graphite_server == "10.0.0.1");
    CHECK(graphite_port == 2003);

    remove(path);
    graphite_server.clear(); graphite_port = 0;
}

static void test_is_nvme()
{
    CHECK(is_nvme("nvme0n1"));
    CHECK(is_nvme("nvme10n2"));
    CHECK(!is_nvme("sda"));
    CHECK(!is_nvme("xnvme"));
}

int main()
{
    test_trim();
    test_split_drive_names();
    test_is_valid_drive_name();
    test_parse_int();
    test_calculate_fan_speed();
    test_validate_settings();
    test_parse_config_file();
    test_is_nvme();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) FAILED.\n", failures);
    return 1;
}
