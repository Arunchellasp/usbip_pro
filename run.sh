#!/bin/bash

# Bash script to run the compiled executable

echo "Closing any existing process on port 5000..."
sudo fuser -k 5000/tcp 2>/dev/null || true

echo "Running the executable..."

# Run the main executable from the build folder with arguments
./build/main "$@"
