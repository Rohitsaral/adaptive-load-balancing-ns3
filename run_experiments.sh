#!/bin/bash
# run_experiments.sh
# Runs all 3 load-balancing algorithms under 2 scenarios (normal traffic,
# and traffic with a mid-simulation server slowdown), saving FlowMonitor
# XML + summary CSV for each into results/.

set -e
cd "$(dirname "$0")/.."   # run from project root

SIM_TIME=40
SERVERS=3
CLIENTS=8
RATE=10

mkdir -p results

for scenario in normal failure; do
  FAIL_FLAG="false"
  if [ "$scenario" == "failure" ]; then FAIL_FLAG="true"; fi

  for algo in round-robin weighted adaptive qlearning; do
    echo ">> Running algo=$algo scenario=$scenario"
    ./adaptive-lb \
      --algo=$algo \
      --simTime=$SIM_TIME \
      --servers=$SERVERS \
      --clients=$CLIENTS \
      --requestRate=$RATE \
      --failure=$FAIL_FLAG \
      --flowmon=results/flow_${algo}_${scenario}.xml \
      --summary=results/summary_${algo}_${scenario}.csv
  done
done

echo ""
echo "All runs complete. Generating plots..."
python3 scripts/analyze_results.py
