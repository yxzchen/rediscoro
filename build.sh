#!/usr/bin/env bash

set -e  # Exit on error

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Project configuration
PROJECT_NAME="rediscoro"
BUILD_DIR="build"
INSTALL_PREFIX="/usr/local"

# Default options
BUILD_TYPE="Debug"
CLEAN=false
INSTALL=false
TEST=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
VERBOSE=false
SANITIZER=""

print_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Options:
    -h, --help              Show this help message
    -d, --debug             Build in Debug mode
    -r, --release           Build in Release mode (default)
    -c, --clean             Clean build directory before building
    -i, --install           Install after building
    -t, --test              Build and run tests after building
    -j, --jobs NUM          Number of parallel build jobs (default: $JOBS)
    -v, --verbose           Show verbose build output
    --asan                  Enable AddressSanitizer (forces Debug)
    --tsan                  Enable ThreadSanitizer (forces Debug)
    --ubsan                 Enable UndefinedBehaviorSanitizer (forces Debug)
    --prefix PATH           Set installation prefix (default: $INSTALL_PREFIX)

Examples:
    $0                      # Build in Release mode
    $0 -d -t                # Build in Debug mode and run tests
    $0 -c -r -i             # Clean, build in Release, and install
    $0 --asan -d -t         # Debug with AddressSanitizer and tests
EOF
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                print_help
                exit 0
                ;;
            -d|--debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            -r|--release)
                BUILD_TYPE="Release"
                shift
                ;;
            -c|--clean)
                CLEAN=true
                shift
                ;;
            -i|--install)
                INSTALL=true
                shift
                ;;
            -t|--test)
                TEST=true
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            --asan)
                SANITIZER="address"
                BUILD_TYPE="Debug"
                shift
                ;;
            --tsan)
                SANITIZER="thread"
                BUILD_TYPE="Debug"
                shift
                ;;
            --ubsan)
                SANITIZER="undefined"
                BUILD_TYPE="Debug"
                shift
                ;;
            --prefix)
                INSTALL_PREFIX="$2"
                shift 2
                ;;
            *)
                log_error "Unknown option: $1"
                print_help
                exit 1
                ;;
        esac
    done
}

check_dependencies() {
    log_info "Checking dependencies..."

    if ! command -v cmake &> /dev/null; then
        log_error "CMake is not installed"
        exit 1
    fi

    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        log_error "No C++ compiler found (g++ or clang++)"
        exit 1
    fi

    log_info "Dependency check passed"
}

clean_build() {
    if [ "$CLEAN" = true ]; then
        log_warn "Cleaning build directory: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
}

configure_project() {
    log_info "Configuring project ($BUILD_TYPE mode)..."

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    CMAKE_ARGS=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    )

    if [ "$TEST" = true ]; then
        CMAKE_ARGS+=(-DREDISCORO_BUILD_TESTS=ON)
    fi

    if [ -n "$SANITIZER" ]; then
        log_info "Enabling ${SANITIZER} sanitizer"
        if [ "$SANITIZER" = "address" ]; then
            CMAKE_ARGS+=(-DREDISCORO_ENABLE_ASAN=ON)
        else
            CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-fsanitize=${SANITIZER} -fno-omit-frame-pointer")
            CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${SANITIZER}")
        fi
    fi

    if [ "$VERBOSE" = true ]; then
        CMAKE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
    fi

    cmake "${CMAKE_ARGS[@]}" ..

    cd ..
}

build_project() {
    log_info "Building project (using $JOBS parallel jobs)..."

    cd "$BUILD_DIR"

    if [ "$VERBOSE" = true ]; then
        cmake --build . -j "$JOBS" -- VERBOSE=1
    else
        cmake --build . -j "$JOBS"
    fi

    cd ..

    log_info "Build completed successfully"
}

run_tests() {
    if [ "$TEST" = true ]; then
        log_info "Running tests..."

        cd "$BUILD_DIR"

        if ! ctest --output-on-failure -j "$JOBS"; then
            log_error "Tests failed"
            exit 1
        fi

        cd ..

        log_info "All tests passed"
    fi
}

install_project() {
    if [ "$INSTALL" = true ]; then
        log_info "Installing to $INSTALL_PREFIX..."

        cd "$BUILD_DIR"

        if [ -w "$INSTALL_PREFIX" ]; then
            cmake --install .
        else
            sudo cmake --install .
        fi

        cd ..

        log_info "Installation completed"
    fi
}

main() {
    parse_args "$@"

    log_info "Starting build for $PROJECT_NAME"
    log_info "Build type: $BUILD_TYPE"

    check_dependencies
    clean_build
    configure_project
    build_project
    run_tests
    install_project

    log_info "All steps completed successfully"

    if [ -d "$BUILD_DIR" ]; then
        log_info "Build artifacts located at: $BUILD_DIR/"
    fi
}

main "$@"


