#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e

echo "--- Setting up PlatformIO Test Environment ---"

# 1. Install PlatformIO Core using pip if it's not already installed
if ! command -v pio &> /dev/null
then
    echo "PlatformIO CLI not found. Installing..."
    pip install -U platformio
else
    echo "PlatformIO CLI is already installed."
fi

echo "--- Running Unit Tests ---"

# 2. Run the unit tests for the 'native' environment
# The `--environment native` flag specifically targets the test configuration
# that runs on the host machine, not the embedded device.
platformio test --environment native

echo "--- Test Execution Finished ---"