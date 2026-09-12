#!/bin/bash

# TerraMaster Fan Control Installation Script
# This script installs the fancontrol service and configuration

# Ensure the script is run with sudo
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root. Use 'sudo' to execute it."
   exit 1
fi

# Define paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY_SOURCE="$SCRIPT_DIR/fancontrol"
BINARY_DEST="/usr/local/bin/fancontrol"
SERVICE_SOURCE="$SCRIPT_DIR/fancontrol.service"
SERVICE_DEST="/etc/systemd/system/fancontrol.service"
CONFIG_SOURCE="$SCRIPT_DIR/fancontrol.conf"
CONFIG_DEST="/etc/fancontrol.conf"

# Check if binary exists
if [[ ! -f "$BINARY_SOURCE" ]]; then
    echo "Error: fancontrol binary not found at $BINARY_SOURCE"
    echo "Please compile the program first:"
    echo "  make"
    echo "or via Docker:"
    echo "  docker run --rm -v \"\$PWD\":/usr/src/myapp -w /usr/src/myapp gcc g++ -O2 -Wall -static -s -o fancontrol fancontrol.cpp"
    exit 1
fi

# Step 1: Stop the service before replacing the binary
# (copying over a running executable fails with "Text file busy")
echo "Stopping fancontrol.service..."
systemctl stop fancontrol.service 2>/dev/null || true
echo "Service stopped (or was not running)."

# Step 2: Copy the binary
echo "Installing fancontrol binary to $BINARY_DEST..."
if cp "$BINARY_SOURCE" "$BINARY_DEST" && chmod +x "$BINARY_DEST"; then
    echo "Successfully installed fancontrol binary."
else
    echo "Failed to install fancontrol binary. Exiting."
    exit 1
fi

# Step 3: Install config file (don't overwrite existing)
if [[ -f "$CONFIG_DEST" ]]; then
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

# Step 4: Copy the service file
echo "Installing service file to $SERVICE_DEST..."
if cp "$SERVICE_SOURCE" "$SERVICE_DEST"; then
    echo "Successfully installed service file."
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
