#!/bin/bash
# build.sh - compiles adaptive-lb.cc against the system-installed NS-3
# (works on Ubuntu with `sudo apt install ns3 libns3-dev`).
set -e
cd "$(dirname "$0")"

g++ -std=c++17 -O2 src/adaptive-lb.cc -o adaptive-lb \
  $(pkg-config --cflags --libs ns3-core ns3-network ns3-internet \
                ns3-point-to-point ns3-applications ns3-flow-monitor)

echo "Build OK -> ./adaptive-lb"
