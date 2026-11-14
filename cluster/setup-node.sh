#!/usr/bin/env bash
# Sets up one Raspberry Pi 4 as a CircleScope node. NOT YET RUN ON REAL HARDWARE.
#
# Start from Raspberry Pi OS Lite (64-bit), then from your Mac:
#   rsync -a --exclude build --exclude external ./ pi@NODE:circlescope/
#   ssh pi@NODE 'cd circlescope && ./cluster/setup-node.sh'
#
# It installs the build tools, builds CircleScopeNode, and installs a service
# that starts a bare X server with the node full screen on every boot.
# Pass --span to draw one picture across both HDMI outputs.
set -euo pipefail
cd "$(dirname "$0")/.."

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates \
    xserver-xorg xinit x11-xserver-utils \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxrender-dev libxcomposite-dev \
    libfreetype-dev libfontconfig1-dev libgl1-mesa-dev mesa-utils

[ -d external/JUCE ] || git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git external/JUCE

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCIRCLESCOPE_NODE_ONLY=ON
cmake --build build -j3 --target CircleScopeNode
sudo install -m 755 build/CircleScopeNode_artefacts/Release/CircleScopeNode /usr/local/bin/CircleScopeNode

# Let a service (not only a console login) start the X server.
printf 'allowed_users=anybody\nneeds_root_rights=yes\n' | sudo tee /etc/X11/Xwrapper.config > /dev/null

sudo tee /usr/local/bin/circlescope-session > /dev/null <<SESSION
#!/bin/sh
xset s off -dpms s noblank
exec /usr/local/bin/CircleScopeNode $*
SESSION
sudo chmod 755 /usr/local/bin/circlescope-session

sudo tee /etc/systemd/system/circlescope-node.service > /dev/null <<UNIT
[Unit]
Description=CircleScope display node
After=network-online.target
Wants=network-online.target

[Service]
User=$USER
SupplementaryGroups=video render input tty
ExecStart=/usr/bin/xinit /usr/local/bin/circlescope-session -- :0 vt7 -nocursor
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now circlescope-node.service
echo "Done. Logs: journalctl -u circlescope-node -f"
