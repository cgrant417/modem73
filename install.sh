#!/usr/bin/env bash
set -e

if [[ $(id -u) -eq 0 ]]; then
    echo "Do not run this script as root. It will ask for sudo when needed."
    exit 1
fi

# Check if this is Debian, Ubuntu, or some other distribution that uses APT.
if command -v apt-get >/dev/null 2>&1; then
    echo "APT-based distribution detected"
    DIST_STYLE="debian"

# Check if this is Arch Linux.
elif command -v pacman >/dev/null 2>&1; then
    echo "Pacman-based distribution detected"
    DIST_STYLE="arch"

# No other distribution styles are supported at this time.
else
    echo "This script requires a Debian-based system with APT, or alternately,"
    echo "Arch Linux. No other distributions are supported yet."
    exit 1
fi

REPO_URL="https://github.com/cgrant417/modem73.git"
BRANCH="modem73fm"

# Build in the checkout this script came from; run on its own, clone modem73fm here
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/kiss_tnc.cc" ] && [ -f "$SCRIPT_DIR/app_info.hh" ]; then
    SRC_DIR="$SCRIPT_DIR"
else
    SRC_DIR="$(pwd)/modem73fm"
fi

echo "=== MODEM73FM Installer ==="
echo "Source directory: $SRC_DIR"
echo ""

# Depending on the distribution style, the naming convention and sources for
# the dependent packages will be different.
if [[ $DIST_STYLE == "debian" ]]; then
    PACKAGES="git build-essential libncurses-dev g++ pkg-config"
elif [[ $DIST_STYLE == "arch" ]]; then
    PACKAGES="git base-devel ncurses pkgconf"
fi

read -rp "Install hamlib for rigctl PTT support? [y/N] " HAMLIB
if [[ "$HAMLIB" =~ ^[Yy]$ ]]; then
    if [[ $DIST_STYLE == "debian" ]]; then
        PACKAGES="$PACKAGES libhamlib-dev libhamlib-utils"
    elif [[ $DIST_STYLE == "arch" ]]; then
        PACKAGES="$PACKAGES hamlib"
    fi
fi

read -rp "Install libhidapi-dev for CM108 USB PTT support? [y/N] " CM108
if [[ "$CM108" =~ ^[Yy]$ ]]; then
    if [[ $DIST_STYLE == "debian" ]]; then
        PACKAGES="$PACKAGES libhidapi-dev"
    elif [[ $DIST_STYLE == "arch" ]]; then
        PACKAGES="$PACKAGES hidapi"
    fi
fi

echo ""
echo "Installing packages: $PACKAGES"

if [[ $DIST_STYLE == "debian" ]]; then
    sudo apt-get update
    sudo apt-get install -y $PACKAGES
elif [[ $DIST_STYLE == "arch" ]]; then
    sudo pacman -Syu --needed $PACKAGES
fi

if [ ! -d "$SRC_DIR" ]; then
    git clone --branch "$BRANCH" "$REPO_URL" "$SRC_DIR"
fi

cd "$SRC_DIR"
make clean 2>/dev/null || true
make -j"$(nproc)"

echo ""
echo "Build complete."

if [[ "$CM108" =~ ^[Yy]$ ]] && ! ldd modem73fm 2>/dev/null | grep -q hidapi; then
    echo ""
    echo "WARNING: you asked for CM108 USB PTT, but the binary is not linked"
    echo "against hidapi, so CM108 is disabled. The Makefile detects hidapi"
    echo "through pkg-config -- check that both pkg-config and the hidapi"
    echo "development headers installed correctly, then rebuild."
fi

echo ""
read -rp "Install modem73fm to /usr/local/bin? [y/N] " INSTALL
if [[ "$INSTALL" =~ ^[Yy]$ ]]; then
    # Some Arch Linux systems, and even some Debian-like ones that have been
    # modified, may not have /usr/local/bin as a valid directory. If that is
    # the case, create it.
    sudo mkdir -p /usr/local/bin
    sudo make install
    echo ""
    echo "Run: modem73fm to launch"
else
    echo ""
    echo "Run: cd $(pwd) && ./modem73fm to launch"
fi
