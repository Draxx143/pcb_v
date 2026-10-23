#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  PCB Viewer — one-shot setup for Linux
#
#  Usage:
#     bash setup_linux.sh              install dependencies (if needed) and build
#     bash setup_linux.sh --cli-only   build only the terminal tool (no Qt needed)
#     bash setup_linux.sh --no-deps    build without installing anything
#     bash setup_linux.sh --run        build and start the GUI afterwards
#     bash setup_linux.sh --desktop    also install a desktop menu entry
#
#  Run it from the repository root:  cd pcb_v && bash setup_linux.sh
# ---------------------------------------------------------------------------
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO_DIR/build"
BUILD_GUI=1
INSTALL_DEPS=1
RUN_AFTER=0
INSTALL_DESKTOP=0

for arg in "$@"; do
    case "$arg" in
        --cli-only)   BUILD_GUI=0 ;;
        --no-deps)    INSTALL_DEPS=0 ;;
        --run)        RUN_AFTER=1 ;;
        --desktop)    INSTALL_DESKTOP=1 ;;
        -h|--help)
            sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "unknown option: $arg (use --help)" >&2
            exit 2 ;;
    esac
done

# ---------- pretty output ----------
if [ -t 1 ]; then
    BOLD=$'\033[1m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
    BOLD=""; GREEN=""; YELLOW=""; RED=""; DIM=""; OFF=""
fi
step() { echo; echo "${BOLD}==> $*${OFF}"; }
info() { echo "    $*"; }
warn() { echo "${YELLOW}    ! $*${OFF}"; }
die()  { echo "${RED}    x $*${OFF}" >&2; exit 1; }

cd "$REPO_DIR" || die "cannot enter $REPO_DIR"

# ---------- sudo helper ----------
SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

need_pkg_install() {
    echo
    echo "${BOLD}The following packages are missing:${OFF} $*"
    read -r -p "Install them now with your package manager? [Y/n] " answer
    answer="${answer:-y}"
    case "$answer" in
        [yY]*) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------- dependency installation ----------
install_deps() {
    local pkgs_missing=0

    command -v g++ >/dev/null 2>&1 || pkgs_missing=1
    command -v cmake >/dev/null 2>&1 || pkgs_missing=1
    local install_cmd=""

    if command -v apt-get >/dev/null 2>&1; then
        install_cmd="apt-get install -y build-essential cmake"
        [ "$BUILD_GUI" -eq 1 ] && install_cmd="$install_cmd qt6-base-dev libgl1-mesa-dev"
    elif command -v dnf >/dev/null 2>&1; then
        install_cmd="dnf install -y gcc-c++ make cmake"
        [ "$BUILD_GUI" -eq 1 ] && install_cmd="$install_cmd qt6-qtbase-devel mesa-libGL-devel"
    elif command -v pacman >/dev/null 2>&1; then
        install_cmd="pacman -S --needed --noconfirm base-devel cmake"
        [ "$BUILD_GUI" -eq 1 ] && install_cmd="$install_cmd qt6-base"
    elif command -v zypper >/dev/null 2>&1; then
        install_cmd="zypper install -y gcc-c++ make cmake"
        [ "$BUILD_GUI" -eq 1 ] && install_cmd="$install_cmd qt6-base-devel Mesa-libGL-devel"
    elif command -v apk >/dev/null 2>&1; then
        install_cmd="apk add --no-cache build-base cmake"
        [ "$BUILD_GUI" -eq 1 ] && install_cmd="$install_cmd qt6-base-dev mesa-dev"
    else
        warn "No supported package manager found (apt/dnf/pacman/zypper/apk)."
        warn "Install a C++17 compiler, CMake and Qt6 development files manually."
        return 0
    fi

    if [ "$pkgs_missing" -eq 0 ] && [ "$BUILD_GUI" -eq 0 ]; then
        return 0
    fi

    step "Installing build dependencies"
    info "$SUDO $install_cmd"
    if $SUDO $install_cmd; then
        echo "${GREEN}    dependencies installed${OFF}"
    else
        warn "Dependency installation failed — trying to build with what is available."
    fi
}

if [ "$INSTALL_DEPS" -eq 1 ]; then
    if ! command -v g++ >/dev/null 2>&1 || ! command -v cmake >/dev/null 2>&1; then
        need_pkg_install "compiler / cmake / Qt6" && install_deps
    elif [ "$BUILD_GUI" -eq 1 ] && ! qmake6 --version >/dev/null 2>&1 && \
         ! pkg-config --exists Qt6Widgets 2>/dev/null && \
         ! ls /usr/lib/*/cmake/Qt6/Qt6Config.cmake /usr/lib/cmake/Qt6/Qt6Config.cmake >/dev/null 2>&1; then
        need_pkg_install "Qt6 development files (qt6-base-dev)" && install_deps
    else
        step "Dependencies"
        info "compiler, cmake and Qt6 are already available — nothing to install."
    fi
fi

# ---------- build ----------
if command -v cmake >/dev/null 2>&1; then
    step "Configuring (CMake)"
    CMAKE_ARGS=(-S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release)
    if [ "$BUILD_GUI" -eq 1 ]; then
        CMAKE_ARGS+=(-DPCBV_BUILD_GUI=ON)
    else
        CMAKE_ARGS+=(-DPCBV_BUILD_GUI=OFF)
    fi
    cmake "${CMAKE_ARGS[@]}" || die "cmake configuration failed"

    step "Building"
    cmake --build "$BUILD_DIR" --parallel "$(nproc 2>/dev/null || echo 2)" \
        || die "build failed"
else
    warn "cmake not found — falling back to a direct g++ build (command line tool only)."
    mkdir -p "$BUILD_DIR" || die "cannot create $BUILD_DIR"
    g++ -std=c++17 -O2 -Isrc \
        src/core/CompoundFile.cpp src/core/AltiumParser.cpp src/core/Export.cpp src/cli/main.cpp \
        -o "$BUILD_DIR/pcbv-cli" || die "g++ build failed"
fi

# ---------- desktop entry ----------
install_desktop_entry() {
    local exe="$BUILD_DIR/pcbv-gui"
    [ -x "$exe" ] || { warn "GUI binary not found — skipping desktop entry."; return 0; }

    local apps_dir="$HOME/.local/share/applications"
    local bin_dir="$HOME/.local/bin"
    mkdir -p "$apps_dir" "$bin_dir" || return 0
    ln -sf "$exe" "$bin_dir/pcbv"
    cat > "$apps_dir/pcb-viewer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PCB Viewer
Comment=Open Altium .PcbDoc / .SchDoc files and list all components
Exec=$exe %f
Terminal=false
Categories=Development;Electronics;Engineering;
MimeType=application/x-altium-pcbdoc;application/x-altium-schdoc;
EOF
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps_dir" >/dev/null 2>&1
    echo "${GREEN}    desktop entry installed${OFF} (menu: PCB Viewer) — command: pcbv"
    info "add ~/.local/bin to PATH if 'pcbv' is not found."
}

if [ "$INSTALL_DESKTOP" -eq 1 ]; then
    step "Installing desktop entry"
    install_desktop_entry
fi

# ---------- summary ----------
step "Done"
if [ -x "$BUILD_DIR/pcbv-gui" ]; then
    echo "    GUI  : ${GREEN}$BUILD_DIR/pcbv-gui${OFF}"
    echo "    CLI  : ${GREEN}$BUILD_DIR/pcbv-cli${OFF}"
    echo
    echo "    start the app            :  ./run.sh"
    echo "    open a file directly     :  ./run.sh /path/to/board.PcbDoc"
    echo "    list components (CLI)    :  ./build/pcbv-cli --list /path/to/board.PcbDoc"
else
    echo "    CLI  : ${GREEN}$BUILD_DIR/pcbv-cli${OFF}"
    echo
    echo "    Qt6 was not available, so the GUI was not built."
    echo "    Install Qt6 (Debian/Ubuntu: sudo apt install qt6-base-dev) and re-run:"
    echo "        bash setup_linux.sh"
    echo
    echo "    meanwhile use the terminal tool:"
    echo "        ./build/pcbv-cli --list /path/to/board.PcbDoc"
fi
echo

if [ "$RUN_AFTER" -eq 1 ] && [ -x "$BUILD_DIR/pcbv-gui" ]; then
    exec "$BUILD_DIR/pcbv-gui" "${PCBV_FILE:-}"
fi
