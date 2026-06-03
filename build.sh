#!/bin/bash

# Bash script to compile C code using GCC

echo "Compiling C code with GCC..."

# Create build folder if it doesn't exist
mkdir -p build

# Compile the main.c file into the build folder
gcc -o build/main code/main.c

# Check if compilation was successful
if [ $? -eq 0 ]; then
    echo "✓ Compilation successful!"
    echo "Executable created: build/main"
else
    echo "✗ Compilation failed!"
    exit 1
fi
