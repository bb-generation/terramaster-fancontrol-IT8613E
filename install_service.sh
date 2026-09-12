#!/bin/bash

# TerraMaster Fan Control Installation Script
# Installs the fancontrol binary, configuration and systemd service.
#
# Usage:
#   sudo ./install_service.sh              Install to /usr/local/bin and /etc
#   sudo ./install_service.sh --in-place   Run the binary and config straight
#                                          out of this directory, install only
#                                          the systemd unit. Use this where the
#                                          root filesystem is read-only, e.g.
#                                          TrueNAS SCALE, and keep this
#                                          directory on a pool.
#
# Destinations can also be overridden individually:
#   sudo BINARY_DEST=/opt/bin/fancontrol ./install_service.sh

set -u

usage() {
    sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
}

# Ensure the script is run with sudo
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root. Use 'sudo' to execute it."
   exit 1
fi

# Define paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY_SOURCE="$SCRIPT_DIR/fancontrol"
SERVICE_SOURCE="$SCRIPT_DIR/fancontrol.service"
CONFIG_SOURCE="$SCRIPT_DIR/fancontrol.conf"

IN_PLACE=false
for arg in "$@"; do
    case "$arg" in
        --in-place) IN_PLACE=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg"; echo; usage; exit 1 ;;
    esac
done

if $IN_PLACE; then
    # Everything stays where it is; only the unit file is installed
    BINARY_DEST="${BINARY_DEST:-$BINARY_SOURCE}"
    CONFIG_DEST="${CONFIG_DEST:-$CONFIG_SOURCE}"
else
    BINARY_DEST="${BINARY_DEST:-/usr/local/bin/fancontrol}"
    CONFIG_DEST="${CONFIG_DEST:-/etc/fancontrol.conf}"
fi
SERVICE_DEST="${SERVICE_DEST:-/etc/systemd/system/fancontrol.service}"

# Check if binary exists
if [[ ! -f "$BINARY_SOURCE" ]]; then
    echo "Error: fancontrol binary not found at $BINARY_SOURCE"
    echo "Please compile the program first:"
    echo "  make"
    echo "or via Docker:"
    echo "  docker run --rm -v \"\$PWD\":/usr/src/myapp -w /usr/src/myapp gcc g++ -O2 -Wall -static -s -o fancontrol fancontrol.cpp"
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "Error: systemctl not found. This script installs a systemd service."
    exit 1
fi

# A read-only root (TrueNAS SCALE and friends) fails the copy halfway through
# otherwise, so check up front and point at --in-place
check_writable() {
    local target="$1" what="$2" dir
    dir="$(dirname "$target")"

    if [[ ! -d "$dir" ]]; then
        echo "Error: $what directory does not exist: $dir"
        return 1
    fi
    if [[ ! -w "$dir" ]]; then
        echo "Error: cannot write $what to $dir (read-only or no permission)."
        if ! $IN_PLACE; then
            echo
            echo "On systems with a read-only root filesystem (e.g. TrueNAS SCALE),"
            echo "keep this directory on a pool and install with:"
            echo "  sudo $0 --in-place"
        fi
        return 1
    fi
    return 0
}

check_writable "$BINARY_DEST" "the binary" || exit 1
check_writable "$CONFIG_DEST" "the configuration file" || exit 1
check_writable "$SERVICE_DEST" "the service file" || exit 1

# Step 1: Stop the service before replacing the binary
# (copying over a running executable fails with "Text file busy")
echo "Stopping fancontrol.service..."
systemctl stop fancontrol.service 2>/dev/null || true
echo "Service stopped (or was not running)."

# Step 2: Install the binary
if [[ "$BINARY_DEST" == "$BINARY_SOURCE" ]]; then
    echo "Using fancontrol binary in place: $BINARY_DEST"
    chmod +x "$BINARY_DEST"
else
    echo "Installing fancontrol binary to $BINARY_DEST..."
    if cp "$BINARY_SOURCE" "$BINARY_DEST" && chmod +x "$BINARY_DEST"; then
        echo "Successfully installed fancontrol binary."
    else
        echo "Failed to install fancontrol binary. Exiting."
        exit 1
    fi
fi

# Step 3: Install config file (don't overwrite existing)
if [[ "$CONFIG_DEST" == "$CONFIG_SOURCE" ]]; then
    echo "Using configuration file in place: $CONFIG_DEST"
elif [[ -f "$CONFIG_DEST" ]]; then
    echo "Configuration file already exists at $CONFIG_DEST, keeping existing config."
    echo "New sample config saved to ${CONFIG_DEST}.new"
    cp "$CONFIG_SOURCE" "${CONFIG_DEST}.new"
else
    echo "Installing configuration file to $CONFIG_DEST..."
    if cp "$CONFIG_SOURCE" "$CONFIG_DEST"; then
        echo "Successfully installed configuration file."
        echo "Edit $CONFIG_DEST to customize settings."
    else
        echo "Failed to install configuration file. Exiting."
        exit 1
    fi
fi

# Step 4: Write the service file with the paths actually used. Binaries on a
# pool need the pool mounted first, so wait for zfs-mount.service there.
echo "Installing service file to $SERVICE_DEST..."
AFTER="local-fs.target"
PROTECT_HOME=yes
for path in "$BINARY_DEST" "$CONFIG_DEST"; do
    case "$path" in
        # Files on a pool need the pool mounted first
        /mnt/*) AFTER="local-fs.target zfs-mount.service" ;;
        # ProtectHome would hide the very files the service has to run
        /home/*|/root/*) PROTECT_HOME=no ;;
    esac
done

if [[ "$PROTECT_HOME" == "no" ]]; then
    echo "Note: files live in a home directory, so ProtectHome is disabled in the unit."
    echo "      TrueNAS SCALE mounts home directories noexec - keep them on a pool instead."
fi

if sed -e "s|^ExecStart=.*|ExecStart=$BINARY_DEST --config=$CONFIG_DEST|" \
       -e "s|^After=.*|After=$AFTER|" \
       -e "s|^ProtectHome=.*|ProtectHome=$PROTECT_HOME|" \
       -e '/^# Option /d' -e '/^# ExecStart=/d' \
       "$SERVICE_SOURCE" > "$SERVICE_DEST"; then
    echo "Successfully installed service file."
    echo "  ExecStart=$BINARY_DEST --config=$CONFIG_DEST"
else
    echo "Failed to install service file. Exiting."
    exit 1
fi

# Step 5: Daemon reload
echo "Reloading systemd daemon..."
if systemctl daemon-reload; then
    echo "Daemon reloaded."
else
    echo "Failed to reload daemon. Exiting."
    exit 1
fi

# Step 6: Start the service
echo "Starting fancontrol.service..."
if systemctl start fancontrol.service; then
    echo "fancontrol.service started successfully."
else
    echo "Failed to start fancontrol.service."
    echo "Check logs with: journalctl -u fancontrol.service"
    exit 1
fi

# Step 7: Enable the service
echo "Enabling fancontrol.service..."
if systemctl enable fancontrol.service; then
    echo "fancontrol.service enabled successfully."
else
    echo "Failed to enable fancontrol.service. Exiting."
    exit 1
fi

# Step 8: Display the service status
echo ""
echo "=========================================="
echo "Installation complete!"
echo "=========================================="
echo ""
echo "Configuration file: $CONFIG_DEST"
echo "Binary location:    $BINARY_DEST"
echo "Service file:       $SERVICE_DEST"
echo ""
echo "Useful commands:"
echo "  View status:  systemctl status fancontrol.service"
echo "  View logs:    journalctl -u fancontrol.service -f"
echo "  Stop:         systemctl stop fancontrol.service"
echo "  Restart:      systemctl restart fancontrol.service"
echo ""
systemctl status fancontrol.service --no-pager

exit 0
