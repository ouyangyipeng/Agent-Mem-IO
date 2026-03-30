#!/bin/bash
# SIFT1M Dataset Download Script
# Downloads the SIFT1M dataset from the official source

set -e

DATA_DIR="${1:-./data}"
SIFT_DIR="${DATA_DIR}/sift1m"

echo "========================================"
echo "SIFT1M Dataset Download Script"
echo "========================================"
echo "Target directory: ${SIFT_DIR}"
echo ""

# Create data directory
mkdir -p "${SIFT_DIR}"
cd "${SIFT_DIR}"

# SIFT1M dataset files
BASE_URL="http://corpus-texmex.irisa.fr/ann-benchmarks/sift1M"

FILES=(
    "sift_base.fvecs"
    "sift_query.fvecs"
    "sift_groundtruth.ivecs"
    "sift_learn.fvecs"
)

# Download files
for file in "${FILES[@]}"; do
    if [ -f "${file}" ]; then
        echo "File ${file} already exists, skipping..."
    else
        echo "Downloading ${file}..."
        wget -q --show-progress "${BASE_URL}/${file}.tar.gz" -O "${file}.tar.gz" || {
            echo "Failed to download ${file}, trying without .tar.gz..."
            wget -q --show-progress "${BASE_URL}/${file}" || {
                echo "ERROR: Failed to download ${file}"
                exit 1
            }
        }
        
        # Extract if tar.gz
        if [ -f "${file}.tar.gz" ]; then
            echo "Extracting ${file}.tar.gz..."
            tar -xzf "${file}.tar.gz"
            rm "${file}.tar.gz"
        fi
    fi
done

echo ""
echo "========================================"
echo "Verifying downloaded files..."
echo "========================================"

# Check file sizes
ls -lh *.fvecs *.ivecs 2>/dev/null || {
    echo "ERROR: No data files found!"
    exit 1
}

echo ""
echo "SIFT1M dataset downloaded successfully!"
echo "Location: ${SIFT_DIR}"
echo ""
echo "Dataset contents:"
echo "  - sift_base.fvecs: 1M base vectors (128 dimensions)"
echo "  - sift_query.fvecs: 10K query vectors"
echo "  - sift_groundtruth.ivecs: Ground truth neighbors"
echo "  - sift_learn.fvecs: 100K learning vectors"