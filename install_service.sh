#!/bin/bash

# TerraMaster Fan Control Installation Script
#
# Installs the systemd service for the fancontrol binary and configuration in
# this directory. Nothing is copied to /usr or /etc: TrueNAS SCALE mounts the
# root filesystem read-only, and files placed there would not survive an update
# anyway. Keep this directory on a pool and re-run this script after a TrueNAS
# update, when the systemd entry is lost.
#
#   sudo ./install_service.sh

set -u

# Ensure the script is run with sudo
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root. Use 'sudo' to execute it."
   exit 1
fi

# Everything runs out of this directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/fancontrol"
CONFIG="$SCRIPT_DIR/fancontrol.conf"
SERVICE_SOURCE="$SCRIPT_DIR/fancontrol.service"
SERVICE_DEST="${SERVICE_DEST:-/etc/systemd/system/fancontrol.service}"

if [[ ! -f "$BINARY" ]]; then
    echo "Error: fancontrol binary not found at $BINARY"
    echo "Please compile the program first:"
    echo "  make"
    echo "or via Docker:"
    echo "  docker run --rm -v \"\$PWD\":/usr/src/myapp -w /usr/src/myapp gcc g++ -O2 -Wall -static -s -o fancontrol fancontrol.cpp"
    exit 1
fi

if [[ ! -f "$CONFIG" ]]; then
    echo "Error: configuration file not found at $CONFIG"
    echo "Generate one with:"
    echo "  ./fancontrol --generate-config=$CONFIG"
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "Error: systemctl not found. This script installs a systemd service."
    exit 1
fi

if [[ ! -w "$(dirname "$SERVICE_DEST")" ]]; then
    echo "Error: cannot write the service file to $(dirname "$SERVICE_DEST")."
    exit 1
fi

# Step 1: Stop the service so it picks up the new binary and config
echo "Stopping fancontrol.service..."
systemctl stop fancontrol.service 2>/dev/null || true
echo "Service stopped (or was not running)."

chmod +x "$BINARY"

# Step 2: Write the service file pointing at this directory.
# ProtectHome would hide the binary from the service if it lives in a home
# directory (where TrueNAS also mounts noexec, so a pool is the right place).
PROTECT_HOME=yes
case "$SCRIPT_DIR" in
    /home/*|/root/*)
        PROTECT_HOME=no
        echo "Note: this directory is inside a home directory, so ProtectHome is disabled."
        echo "      TrueNAS SCALE mounts home directories noexec - move this to a pool."
        ;;
esac

echo "Installing service file to $SERVICE_DEST..."
if sed -e "s|^ExecStart=.*|ExecStart=$BINARY --config=$CONFIG|" \
       -e "s|^ProtectHome=.*|ProtectHome=$PROTECT_HOME|" \
       "$SERVICE_SOURCE" > "$SERVICE_DEST"; then
    echo "Successfully installed service file."
    echo "  ExecStart=$BINARY --config=$CONFIG"
else
    echo "Failed to install service file. Exiting."
    exit 1
fi

# Step 3: Daemon reload
echo "Reloading systemd daemon..."
if systemctl daemon-reload; then
    echo "Daemon reloaded."
else
    echo "Failed to reload daemon. Exiting."
    exit 1
fi

# Step 4: Start the service
echo "Starting fancontrol.service..."
if systemctl start fancontrol.service; then
    echo "fancontrol.service started successfully."
else
    echo "Failed to start fancontrol.service."
    echo "Check logs with: journalctl -u fancontrol.service"
    exit 1
fi

# Step 5: Enable the service
echo "Enabling fancontrol.service..."
if systemctl enable fancontrol.service; then
    echo "fancontrol.service enabled successfully."
else
    echo "Failed to enable fancontrol.service. Exiting."
    exit 1
fi

# Step 6: Display the service status
echo ""
echo "=========================================="
echo "Installation complete!"
echo "=========================================="
echo ""
echo "Binary:             $BINARY"
echo "Configuration file: $CONFIG"
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
