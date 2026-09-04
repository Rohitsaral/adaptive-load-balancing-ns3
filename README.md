# Adaptive Load Balancing Using Intelligent Routing in Computer Networks
### (NS-3 implementation)

A final-year networking project built on **NS-3 (v3.41)**, a real
discrete-event network simulator, comparing traditional load-balancing
algorithms against an **adaptive/intelligent router** that makes
routing decisions using live network-layer signals.

---

## 1. Topology

Star topology with the Load Balancer (LB) as the central hub:

```
 client0 ─┐                                  ┌─ server0 (strong)
 client1 ─┤   P2P links, 10Mbps/5ms          │  P2P links, 20Mbps/2ms
 client2 ─┼───────────────[ Load Balancer ]──┼─ server1 (medium)
   ...    │                                  │
 clientN ─┘                                  └─ serverM (weaker/legacy)
```

- Each **client** sends periodic UDP "requests" (Poisson arrivals) to
  the LB's well-known port.
- The **LoadBalancerApp** (custom NS-3 application, `src/adaptive-lb.cc`)
  receives each request, picks a backend using the selected algorithm,
  relays it, and relays the eventual response back to the client —
  a standard reverse-proxy relay model (like HAProxy/nginx).
- Each **BackendServerApp** simulates processing delay (exponentially
  distributed, mean = 1 / service-rate) before responding.
- **Servers are heterogeneous**: server0 is fastest, each subsequent
  server progressively slower — modeling a real, non-uniform data
  center.

## 2. Algorithms Implemented

| Algorithm | How it decides | Adapts to real-time conditions? | Type |
|---|---|---|---|
| **Round Robin** | Cycles through servers in fixed order | No | Baseline |
| **Weighted Round Robin** | Static weights ∝ each server's declared capacity | No — weights never change at runtime | Static heuristic |
| **Adaptive (Heuristic)** | Reads the **live NS-3 point-to-point NetDevice queue occupancy** + in-flight requests per server, picks the least-congested | Yes — every decision, using a hand-designed scoring rule | Rule-based |
| **Intelligent (Q-Learning)** | **Reinforcement Learning** — learns, purely from trial and reward, which server tends to give the best response time under which congestion pattern | Yes — continuously improves its own policy from experience | **Machine Learning** |

### Why the adaptive & Q-learning algorithms are genuinely "intelligent routing"

The heuristic **Adaptive** router queries **real NS-3 network objects** at
every decision:

```cpp
Ptr<Queue<Packet>> q = m_serverDevices[i]->GetQueue();
uint32_t queueLen = q->GetNPackets();       // actual link congestion
uint32_t score = queueLen * 10 + m_activeConn[i];  // + outstanding requests
// route to the server with the lowest score
```

The **Q-Learning** router goes a step further — it doesn't use a
hand-designed formula at all. It learns entirely from experience:

- **State** — congestion level (low/medium/high, from queue length +
  in-flight requests) of every server, encoded as one integer.
- **Action** — which server to route to.
- **Reward** — negative observed response time of the completed request.
- **Policy** — epsilon-greedy: mostly exploits its current best guess,
  occasionally explores a different server so it keeps discovering
  better routes, with exploration decaying over time.
- **Update rule** (standard Q-learning):
  `Q(s,a) += alpha * (reward + gamma * max_a' Q(s,a') - Q(s,a))`

Both routers react to the same real network-layer signals; the
difference is that Adaptive uses a rule *we* designed, while Q-Learning
*discovers its own routing policy* purely from trial-and-reward — a
genuine machine-learning approach to the same problem.

## 3. Metrics Collected

- **Application-level RTT** — each client times its own
  request→response round trip.
- **Packet loss %** — requests sent vs. responses received.
- **Load distribution** — requests routed to each server.
- **ns3::FlowMonitor** — standard network-layer delay, jitter, and
  throughput per flow, exported to XML, as an independent cross-check
  of the application-level numbers.

## 4. Project Structure

```
ns3_project/
├── src/
│   └── adaptive-lb.cc        # Main NS-3 simulation (LB + client + server apps)
├── scripts/
│   ├── run_experiments.sh    # Runs all 3 algorithms × 2 scenarios
│   └── analyze_results.py    # Parses CSV/XML, generates comparison plots
├── build.sh                  # Compiles adaptive-lb.cc
├── results/                  # Generated CSV, FlowMonitor XML, PNG plots
└── README.md
```

## 5. How to Build & Run

### Option A — system NS-3 package (what this project was built/tested with)

```bash
sudo apt install ns3 libns3-dev libgsl-dev libsqlite3-dev
./build.sh
./scripts/run_experiments.sh      # runs all experiments + generates plots
```

Or run a single configuration manually:

```bash
./adaptive-lb --algo=adaptive --servers=3 --clients=8 --requestRate=10 \
              --simTime=40 --failure=true \
              --flowmon=results/flow.xml --summary=results/summary.csv
```

**Command-line options:**

| Flag | Meaning | Default |
|---|---|---|
| `--algo` | `round-robin` \| `weighted` \| `adaptive` \| `qlearning` | `adaptive` |
| `--servers` | number of backend servers | 3 |
| `--clients` | number of client nodes | 6 |
| `--simTime` | simulation duration (s) | 60 |
| `--requestRate` | requests/sec **per client** | 8 |
| `--failure` | inject a mid-run slowdown on server0 | false |
| `--flowmon` | FlowMonitor XML output path | results/flow.xml |
| `--summary` | summary CSV output path | results/summary.csv |

### Option B — full source-tree NS-3 (typical college `ns-3-dev` setup)

If your lab uses the source-built NS-3 (cloned from the official repo
and built with `./ns3` or `./waf`), just drop `adaptive-lb.cc` into
your `ns-3-dev/scratch/` folder and run:

```bash
cp src/adaptive-lb.cc  <ns-3-dev>/scratch/
cd <ns-3-dev>
./ns3 run "scratch/adaptive-lb --algo=adaptive --failure=true"
```

(The code only uses standard modules — core, network, internet,
point-to-point, applications, flow-monitor — so it needs no extra
NS-3 modules enabled.)

## 6. Key Result — Server Degradation Scenario

Traffic: 8 clients × 10 req/s, 3 heterogeneous servers, server0's
processing rate cut drastically between t=14s and t=28s of a 40s run.

| Algorithm | Avg App RTT | Packet Loss | Behavior |
|---|---|---|---|
| Round Robin | 74.5 ms | 0.03% | Keeps sending 1/3 of traffic to the slow server regardless |
| Weighted Round Robin | 87.1 ms | 0.06% | **Worse than Round Robin** — its static weight assumes server0 is *strong*, so it sends it *more* traffic, unaware it has degraded |
| Adaptive (Heuristic) | 47.3 ms | 0.03% | Detects rising queue/in-flight requests on server0 and shifts traffic to healthy servers |
| **Intelligent (Q-Learning)** | **42.7 ms** | **0.03%** | Learns the degradation from reward feedback and shifts traffic even more decisively (792 requests to the failing server0 vs. 1046 under Round Robin) — best result of all four algorithms |

This is the project's central finding: **static algorithms can't react
to changing conditions — a naive "smarter" static policy (weighted) can
even backfire — while both the rule-based adaptive router and the
learning-based Q-learning router recover performance automatically,
with the ML-based router edging out the hand-designed heuristic.**

Run `scripts/run_experiments.sh` to regenerate this comparison; plots
are saved as `results/ns3_comparison_normal.png` and
`results/ns3_comparison_failure.png`.

## 7. Viva / Report Talking Points

- **Why point-to-point queue occupancy is a valid congestion signal**:
  it directly reflects packets waiting to be transmitted on that
  link/towards that server — a real NS-3 network object, not a
  simulated guess.
- **Why "in-flight requests" (active connections) matters too**: queue
  occupancy alone only captures *link* congestion; a server that is
  slow to *process* (but not congesting the link) is caught by the
  rising count of unanswered requests — the same idea as "least
  outstanding requests" balancing used in real systems like Envoy.
- **Why Weighted Round Robin can be *worse* than plain Round Robin
  under failure**: static weights encode an assumption about capacity
  that becomes wrong the moment conditions change — a good example for
  explaining the difference between *static* and *adaptive* systems.
- **FlowMonitor vs. application-level RTT**: FlowMonitor measures
  network-layer (IP) delay per flow; the app-level RTT additionally
  includes server processing time — comparing both shows where time is
  actually being spent (mostly server processing delay here, not the
  network).

## 8. Possible Extensions

- Replace the tabular Q-learning with a small neural network (Deep
  Q-Network) for larger server pools, where the number of discretized
  states grows too big for a plain table.
- Add TCP-based traffic (BulkSendApplication) instead of UDP to study
  interaction with TCP congestion control.
- Scale to a multi-tier topology (multiple LB instances behind a DNS
  round-robin layer).
- Visualize the topology and live packet flow using **NetAnim**.
