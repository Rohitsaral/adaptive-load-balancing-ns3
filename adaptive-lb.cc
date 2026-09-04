/*
 * adaptive-lb.cc
 * ---------------------------------------------------------------------
 * Adaptive Load Balancing Using Intelligent Routing in Computer Networks
 * ---------------------------------------------------------------------
 * Topology (star, LB as central hub):
 *
 *     client0 --\                              /-- server0
 *     client1 ---\    (point-to-point links)  /--- server1     (P2P links)
 *     client2 ----+---------[ LoadBalancer ]--+---- server2
 *     ...        /                             \
 *     clientN-1-/                               \-- serverM-1
 *
 * Each client sends periodic UDP "requests" to the LoadBalancer node's
 * well-known port. The LoadBalancer node runs a custom relay
 * application (LoadBalancerApp) that:
 *   1. Picks a backend server according to the selected algorithm
 *      (--algo=round-robin | weighted | adaptive)
 *   2. Forwards the request to that server
 *   3. Relays the server's response back to the original client
 *
 * The "adaptive" algorithm is the project's core contribution: at every
 * routing decision it reads REAL network-layer signals -- the current
 * queue occupancy of the point-to-point NetDevice towards each
 * candidate server (i.e. actual congestion on that link/server) -- and
 * routes to the least-congested one. This reacts to real traffic
 * conditions and server slowdowns, unlike Round Robin / static
 * Weighted Round Robin.
 *
 * ns3::FlowMonitor is attached to capture standard network metrics
 * (delay, jitter, throughput, packet loss) per flow, dumped to XML for
 * offline analysis (see scripts/analyze_results.py).
 * ---------------------------------------------------------------------
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <map>
#include <vector>
#include <algorithm>
#include <cstring>
#include <fstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("AdaptiveLoadBalancing");

// =========================================================================
// Wire format helpers: every UDP payload starts with a 4-byte big-endian
// uint32_t "id" field followed by application payload bytes.
// =========================================================================
static Ptr<Packet> MakePacketWithId(uint32_t id, const uint8_t *body, uint32_t bodyLen) {
  std::vector<uint8_t> buf(4 + bodyLen);
  buf[0] = (id >> 24) & 0xff;
  buf[1] = (id >> 16) & 0xff;
  buf[2] = (id >> 8) & 0xff;
  buf[3] = id & 0xff;
  if (bodyLen > 0) std::memcpy(buf.data() + 4, body, bodyLen);
  return Create<Packet>(buf.data(), buf.size());
}

static uint32_t ReadIdFromPacket(Ptr<Packet> pkt, std::vector<uint8_t> *bodyOut = nullptr) {
  uint32_t size = pkt->GetSize();
  std::vector<uint8_t> buf(size);
  pkt->CopyData(buf.data(), size);
  uint32_t id = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
  if (bodyOut && size > 4) {
    bodyOut->assign(buf.begin() + 4, buf.end());
  }
  return id;
}

// =========================================================================
// Routing algorithm enum
// =========================================================================
enum class LbAlgo { ROUND_ROBIN, WEIGHTED, ADAPTIVE, QLEARNING };

static LbAlgo ParseAlgo(const std::string &s) {
  if (s == "round-robin") return LbAlgo::ROUND_ROBIN;
  if (s == "weighted")    return LbAlgo::WEIGHTED;
  if (s == "adaptive")    return LbAlgo::ADAPTIVE;
  if (s == "qlearning")   return LbAlgo::QLEARNING;
  NS_FATAL_ERROR("Unknown --algo value: " << s
                  << " (use round-robin | weighted | adaptive | qlearning)");
}

// =========================================================================
// BackendServerApp : simulates a server that takes some processing time
// (exponentially distributed, mean = 1/serviceRate) before replying.
// =========================================================================
class BackendServerApp : public Application {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("BackendServerApp")
      .SetParent<Application>()
      .AddConstructor<BackendServerApp>();
    return tid;
  }

  void Setup(uint16_t port, double serviceRate) {
    m_port = port;
    m_serviceRate = serviceRate;
    m_rand = CreateObject<ExponentialRandomVariable>();
    m_rand->SetAttribute("Mean", DoubleValue(1.0 / serviceRate));
  }

  // Allows the simulation script to change this server's processing
  // speed at runtime (used to simulate a slowdown / partial failure).
  void SetServiceRate(double serviceRate) {
    m_serviceRate = serviceRate;
    m_rand->SetAttribute("Mean", DoubleValue(1.0 / serviceRate));
  }

  uint64_t GetRequestsServed() const { return m_requestsServed; }

private:
  virtual void StartApplication() override {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    InetSocketAddress local(Ipv4Address::GetAny(), m_port);
    m_socket->Bind(local);
    m_socket->SetRecvCallback(MakeCallback(&BackendServerApp::HandleRead, this));
  }

  virtual void StopApplication() override {
    if (m_socket) m_socket->Close();
  }

  void HandleRead(Ptr<Socket> socket) {
    Address from;
    Ptr<Packet> packet;
    while ((packet = socket->RecvFrom(from))) {
      uint32_t id = ReadIdFromPacket(packet);
      // simulate variable processing time before responding
      double delay = m_rand->GetValue();
      Simulator::Schedule(Seconds(delay), &BackendServerApp::SendResponse,
                           this, id, from);
    }
  }

  void SendResponse(uint32_t id, Address to) {
    Ptr<Packet> resp = MakePacketWithId(id, nullptr, 0);
    m_socket->SendTo(resp, 0, to);
    m_requestsServed++;
  }

  Ptr<Socket> m_socket;
  uint16_t m_port = 0;
  double m_serviceRate = 1.0;
  Ptr<ExponentialRandomVariable> m_rand;
  uint64_t m_requestsServed = 0;
};

// =========================================================================
// LoadBalancerApp : the core of the project. Receives client requests,
// picks a backend using the configured algorithm, relays the request,
// and relays the eventual response back to the client.
// =========================================================================
class LoadBalancerApp : public Application {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("LoadBalancerApp")
      .SetParent<Application>()
      .AddConstructor<LoadBalancerApp>();
    return tid;
  }

  // Must be called before StartApplication (i.e. right after node creation)
  void Setup(uint16_t clientPort, LbAlgo algo,
             const std::vector<Ipv4Address> &serverAddrs,
             uint16_t serverPort,
             const std::vector<Ptr<PointToPointNetDevice>> &serverDevices,
             const std::vector<double> &serverWeights) {
    m_clientPort = clientPort;
    m_algo = algo;
    m_serverAddrs = serverAddrs;
    m_serverPort = serverPort;
    m_serverDevices = serverDevices;
    m_serverWeights = serverWeights;
    m_activeConn.assign(serverAddrs.size(), 0);
    m_wrrCounters.assign(serverAddrs.size(), 0.0);
  }

  // total requests routed to each server -- used for the load-distribution report
  const std::vector<uint64_t> &GetRoutedCounts() const { return m_routedCounts; }

private:
  virtual void StartApplication() override {
    m_routedCounts.assign(m_serverAddrs.size(), 0);

    // socket facing the clients
    m_clientSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_clientSocket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_clientPort));
    m_clientSocket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleClientRequest, this));

    // one connected socket per backend server
    for (size_t i = 0; i < m_serverAddrs.size(); ++i) {
      Ptr<Socket> s = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
      s->Bind();
      s->Connect(InetSocketAddress(m_serverAddrs[i], m_serverPort));
      s->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleServerResponseDispatch, this));
      m_serverSockets.push_back(s);
    }
  }

  virtual void StopApplication() override {
    if (m_clientSocket) m_clientSocket->Close();
    for (auto &s : m_serverSockets) s->Close();
  }

  void HandleServerResponseDispatch(Ptr<Socket> socket) {
    // find index of this socket
    size_t idx = 0;
    for (size_t i = 0; i < m_serverSockets.size(); ++i) {
      if (m_serverSockets[i] == socket) { idx = i; break; }
    }
    HandleServerResponse(socket, idx);
  }

  void HandleClientRequest(Ptr<Socket> socket) {
    Address from;
    Ptr<Packet> packet;
    while ((packet = socket->RecvFrom(from))) {
      std::vector<uint8_t> body;
      uint32_t clientId = ReadIdFromPacket(packet, &body);

      uint32_t lbId = m_nextLbId++;

      int qState = -1;
      size_t idx = ChooseServer(qState);   // qState filled in only for QLEARNING

      m_pending[lbId] = {from, clientId, idx, qState, Simulator::Now()};

      m_activeConn[idx]++;
      m_routedCounts[idx]++;

      Ptr<Packet> fwd = MakePacketWithId(lbId, body.data(), body.size());
      m_serverSockets[idx]->Send(fwd);
    }
  }

  void HandleServerResponse(Ptr<Socket> socket, size_t idx) {
    Address dummy;
    Ptr<Packet> packet;
    while ((packet = socket->RecvFrom(dummy))) {
      uint32_t lbId = ReadIdFromPacket(packet);
      auto it = m_pending.find(lbId);
      if (it == m_pending.end()) continue;  // stale/unknown, drop

      m_activeConn[idx] = (m_activeConn[idx] > 0) ? m_activeConn[idx] - 1 : 0;

      // ---- Q-learning feedback: reward = -observed response time --------
      if (m_algo == LbAlgo::QLEARNING && it->second.qState >= 0) {
        double rtt = (Simulator::Now() - it->second.sendTime).GetSeconds();
        UpdateQ(it->second.qState, static_cast<int>(idx), -rtt);
      }

      Ptr<Packet> resp = MakePacketWithId(it->second.clientId, nullptr, 0);
      m_clientSocket->SendTo(resp, 0, it->second.clientAddr);
      m_pending.erase(it);
    }
  }

  // ---- the four routing strategies -----------------------------------
  // qStateOut is set only when the QLEARNING algorithm is used (so the
  // caller can remember which state/action pair to credit on response).
  size_t ChooseServer(int &qStateOut) {
    qStateOut = -1;
    switch (m_algo) {
      case LbAlgo::ROUND_ROBIN:  return ChooseRoundRobin();
      case LbAlgo::WEIGHTED:     return ChooseWeighted();
      case LbAlgo::ADAPTIVE:     return ChooseAdaptive();
      case LbAlgo::QLEARNING:    return ChooseQLearning(qStateOut);
    }
    return 0;
  }

  size_t ChooseRoundRobin() {
    size_t idx = m_rrIndex % m_serverAddrs.size();
    m_rrIndex++;
    return idx;
  }

  size_t ChooseWeighted() {
    // pick server with the smallest (served / weight) ratio so far
    size_t best = 0;
    double bestRatio = m_wrrCounters[0] / m_serverWeights[0];
    for (size_t i = 1; i < m_serverAddrs.size(); ++i) {
      double ratio = m_wrrCounters[i] / m_serverWeights[i];
      if (ratio < bestRatio) { bestRatio = ratio; best = i; }
    }
    m_wrrCounters[best] += 1.0;
    return best;
  }

  size_t ChooseAdaptive() {
    // INTELLIGENT ROUTING: read the *actual* current queue occupancy of
    // the point-to-point NetDevice heading to each server. This is a
    // real, live network-layer congestion signal (not a static guess),
    // so the router automatically avoids servers that are currently
    // backed up -- including a server that has slowed down or partially
    // failed mid-simulation.
    size_t best = 0;
    uint32_t bestScore = std::numeric_limits<uint32_t>::max();
    for (size_t i = 0; i < m_serverDevices.size(); ++i) {
      Ptr<Queue<Packet>> q = m_serverDevices[i]->GetQueue();
      uint32_t queueLen = q ? q->GetNPackets() : 0;
      // combine queue length with in-flight (unanswered) request count
      // for a more responsive congestion estimate
      uint32_t score = queueLen * 10 + static_cast<uint32_t>(m_activeConn[i]);
      if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
  }

  // ======================================================================
  // MACHINE LEARNING: Q-Learning based intelligent router
  //
  // STATE  : the congestion level (LOW/MEDIUM/HIGH bucket, based on
  //          queue length + in-flight requests) of every server,
  //          encoded as a single integer (base-N_BUCKETS positional
  //          encoding) -> captures the overall network picture.
  // ACTION : which server to route the next request to.
  // REWARD : -1 * observed response time of the completed request
  //          (so the agent is trained to MINIMIZE response time).
  //
  // The Q-table starts empty (all zeros) and is updated online, after
  // every completed request, using the standard Q-learning rule:
  //     Q(s,a) <- Q(s,a) + alpha * [ r + gamma * max_a' Q(s,a') - Q(s,a) ]
  // Decisions use an epsilon-greedy policy so the agent keeps exploring
  // (trying other servers) instead of getting stuck on an early guess,
  // with epsilon decaying over time as its estimates improve.
  // ======================================================================
  static constexpr int N_BUCKETS = 3;   // 0=low, 1=medium, 2=high congestion

  int CongestionBucket(size_t i) const {
    Ptr<Queue<Packet>> q = m_serverDevices[i]->GetQueue();
    uint32_t queueLen = q ? q->GetNPackets() : 0;
    int64_t score = queueLen + m_activeConn[i];
    if (score < 2) return 0;
    if (score < 5) return 1;
    return 2;
  }

  int EncodeState() const {
    int state = 0;
    for (size_t i = 0; i < m_serverAddrs.size(); ++i) {
      state = state * N_BUCKETS + CongestionBucket(i);
    }
    return state;
  }

  std::vector<double> &QRow(int state) {
    auto it = m_qtable.find(state);
    if (it == m_qtable.end()) {
      it = m_qtable.emplace(state, std::vector<double>(m_serverAddrs.size(), 0.0)).first;
    }
    return it->second;
  }

  size_t ChooseQLearning(int &qStateOut) {
    int state = EncodeState();
    qStateOut = state;
    std::vector<double> &qRow = QRow(state);

    size_t action;
    if (m_uniformRand->GetValue(0.0, 1.0) < m_epsilon) {
      action = static_cast<size_t>(m_uniformRand->GetInteger(0, m_serverAddrs.size() - 1));  // explore
    } else {
      action = 0;
      double best = qRow[0];
      for (size_t i = 1; i < qRow.size(); ++i) {
        if (qRow[i] > best) { best = qRow[i]; action = i; }
      }
    }
    m_epsilon = std::max(m_epsilonMin, m_epsilon * m_epsilonDecay);
    return action;
  }

  void UpdateQ(int state, int action, double reward) {
    std::vector<double> &qRow = QRow(state);
    double maxNext = *std::max_element(qRow.begin(), qRow.end());
    double &q = qRow[action];
    q = q + m_alpha * (reward + m_gamma * maxNext - q);
  }

  struct PendingRequest {
    Address clientAddr;
    uint32_t clientId;
    size_t serverIdx;      // which server this request was routed to
    int qState;            // Q-learning state at decision time (-1 if N/A)
    Time sendTime;          // when the LB forwarded this request to the server
  };

  uint16_t m_clientPort = 0;
  uint16_t m_serverPort = 0;
  LbAlgo m_algo = LbAlgo::ROUND_ROBIN;

  std::vector<Ipv4Address> m_serverAddrs;
  std::vector<Ptr<PointToPointNetDevice>> m_serverDevices;
  std::vector<double> m_serverWeights;
  std::vector<double> m_wrrCounters;
  std::vector<int64_t> m_activeConn;
  std::vector<uint64_t> m_routedCounts;

  Ptr<Socket> m_clientSocket;
  std::vector<Ptr<Socket>> m_serverSockets;

  size_t m_rrIndex = 0;
  uint32_t m_nextLbId = 1;
  std::map<uint32_t, PendingRequest> m_pending;

  // Q-learning state
  std::map<int, std::vector<double>> m_qtable;
  double m_alpha = 0.3;          // learning rate
  double m_gamma = 0.5;          // discount factor
  double m_epsilon = 0.3;        // exploration rate (starts high)
  double m_epsilonDecay = 0.995;
  double m_epsilonMin = 0.05;
  Ptr<UniformRandomVariable> m_uniformRand = CreateObject<UniformRandomVariable>();
};

// =========================================================================
// LBClientApp : generates Poisson-arrival requests towards the LB and
// measures end-to-end (application-level) round-trip time.
// =========================================================================
class LBClientApp : public Application {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("LBClientApp")
      .SetParent<Application>()
      .AddConstructor<LBClientApp>();
    return tid;
  }

  void Setup(Ipv4Address lbAddr, uint16_t lbPort, double requestRate, double stopTime) {
    m_lbAddr = lbAddr;
    m_lbPort = lbPort;
    m_rand = CreateObject<ExponentialRandomVariable>();
    m_rand->SetAttribute("Mean", DoubleValue(1.0 / requestRate));
    m_stopTime = stopTime;
  }

  double GetAvgRtt() const {
    return m_rttCount ? (m_rttSum / m_rttCount) : 0.0;
  }
  uint64_t GetRequestsSent() const { return m_sent; }
  uint64_t GetResponsesReceived() const { return m_rttCount; }

private:
  virtual void StartApplication() override {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind();
    m_socket->Connect(InetSocketAddress(m_lbAddr, m_lbPort));
    m_socket->SetRecvCallback(MakeCallback(&LBClientApp::HandleRead, this));
    ScheduleNext();
  }

  virtual void StopApplication() override {
    Simulator::Cancel(m_nextEvent);
    if (m_socket) m_socket->Close();
  }

  void ScheduleNext() {
    double delay = m_rand->GetValue();
    if (Simulator::Now().GetSeconds() + delay >= m_stopTime) return;
    m_nextEvent = Simulator::Schedule(Seconds(delay), &LBClientApp::SendRequest, this);
  }

  void SendRequest() {
    uint32_t id = m_nextId++;
    m_sendTime[id] = Simulator::Now();
    Ptr<Packet> pkt = MakePacketWithId(id, nullptr, 0);
    m_socket->Send(pkt);
    m_sent++;
    ScheduleNext();
  }

  void HandleRead(Ptr<Socket> socket) {
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from))) {
      uint32_t id = ReadIdFromPacket(packet);
      auto it = m_sendTime.find(id);
      if (it == m_sendTime.end()) continue;
      double rtt = (Simulator::Now() - it->second).GetSeconds();
      m_rttSum += rtt;
      m_rttCount++;
      m_sendTime.erase(it);
    }
  }

  Ptr<Socket> m_socket;
  Ipv4Address m_lbAddr;
  uint16_t m_lbPort = 0;
  Ptr<ExponentialRandomVariable> m_rand;
  double m_stopTime = 0;
  EventId m_nextEvent;
  uint32_t m_nextId = 1;
  uint64_t m_sent = 0;
  std::map<uint32_t, Time> m_sendTime;
  double m_rttSum = 0;
  uint64_t m_rttCount = 0;
};

// =========================================================================
// main
// =========================================================================
int main(int argc, char *argv[]) {
  std::string algoStr = "adaptive";
  uint32_t nServers = 3;
  uint32_t nClients = 6;
  double simTime = 60.0;
  double requestRate = 8.0;      // requests/sec PER client
  bool injectFailure = false;
  std::string flowMonFile = "results/flow.xml";
  std::string summaryFile = "results/summary.csv";

  CommandLine cmd;
  cmd.AddValue("algo", "round-robin | weighted | adaptive | qlearning", algoStr);
  cmd.AddValue("servers", "number of backend servers", nServers);
  cmd.AddValue("clients", "number of client nodes", nClients);
  cmd.AddValue("simTime", "simulation duration (s)", simTime);
  cmd.AddValue("requestRate", "requests/sec per client", requestRate);
  cmd.AddValue("failure", "inject a mid-simulation server slowdown", injectFailure);
  cmd.AddValue("flowmon", "FlowMonitor XML output path", flowMonFile);
  cmd.AddValue("summary", "summary CSV output path", summaryFile);
  cmd.Parse(argc, argv);

  LbAlgo algo = ParseAlgo(algoStr);

  // ---- heterogeneous server "power": capacity-ish weight + service rate
  // server 0 = strong, server 1 = medium, others progressively weaker
  std::vector<double> baseServiceRates;
  std::vector<double> weights;
  for (uint32_t i = 0; i < nServers; ++i) {
    double rate = 60.0 - i * 15.0;         // requests/sec this server can process
    if (rate < 10.0) rate = 10.0;
    baseServiceRates.push_back(rate);
    weights.push_back(rate);                // weight proportional to capacity
  }

  // ---- nodes -----------------------------------------------------------
  NodeContainer lbNode; lbNode.Create(1);
  NodeContainer serverNodes; serverNodes.Create(nServers);
  NodeContainer clientNodes; clientNodes.Create(nClients);

  InternetStackHelper stack;
  stack.Install(lbNode);
  stack.Install(serverNodes);
  stack.Install(clientNodes);

  PointToPointHelper p2pServer;
  p2pServer.SetDeviceAttribute("DataRate", StringValue("20Mbps"));
  p2pServer.SetChannelAttribute("Delay", StringValue("2ms"));

  PointToPointHelper p2pClient;
  p2pClient.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2pClient.SetChannelAttribute("Delay", StringValue("5ms"));

  Ipv4AddressHelper address;

  // LB <-> servers
  std::vector<Ipv4Address> serverAddrs;
  std::vector<Ptr<PointToPointNetDevice>> serverDevicesOnLb;
  for (uint32_t i = 0; i < nServers; ++i) {
    NetDeviceContainer link = p2pServer.Install(lbNode.Get(0), serverNodes.Get(i));
    std::ostringstream subnet;
    subnet << "10.1." << (i + 1) << ".0";
    address.SetBase(subnet.str().c_str(), "255.255.255.0");
    Ipv4InterfaceContainer iface = address.Assign(link);
    serverAddrs.push_back(iface.GetAddress(1));  // server side
    serverDevicesOnLb.push_back(DynamicCast<PointToPointNetDevice>(link.Get(0)));
  }

  // LB <-> clients
  std::vector<Ipv4Address> lbAddrsForClients;
  for (uint32_t i = 0; i < nClients; ++i) {
    NetDeviceContainer link = p2pClient.Install(lbNode.Get(0), clientNodes.Get(i));
    std::ostringstream subnet;
    subnet << "10.2." << (i + 1) << ".0";
    address.SetBase(subnet.str().c_str(), "255.255.255.0");
    Ipv4InterfaceContainer iface = address.Assign(link);
    lbAddrsForClients.push_back(iface.GetAddress(0));  // LB side address on this link
  }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // ---- applications ------------------------------------------------
  const uint16_t serverPort = 5000;
  const uint16_t lbClientPort = 9000;

  ApplicationContainer serverApps;
  std::vector<Ptr<BackendServerApp>> serverAppPtrs;
  for (uint32_t i = 0; i < nServers; ++i) {
    Ptr<BackendServerApp> app = CreateObject<BackendServerApp>();
    app->Setup(serverPort, baseServiceRates[i]);
    serverNodes.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(0.0));
    app->SetStopTime(Seconds(simTime));
    serverAppPtrs.push_back(app);
    serverApps.Add(app);
  }

  Ptr<LoadBalancerApp> lbApp = CreateObject<LoadBalancerApp>();
  lbApp->Setup(lbClientPort, algo, serverAddrs, serverPort, serverDevicesOnLb, weights);
  lbNode.Get(0)->AddApplication(lbApp);
  lbApp->SetStartTime(Seconds(0.0));
  lbApp->SetStopTime(Seconds(simTime));

  std::vector<Ptr<LBClientApp>> clientAppPtrs;
  for (uint32_t i = 0; i < nClients; ++i) {
    Ptr<LBClientApp> app = CreateObject<LBClientApp>();
    app->Setup(lbAddrsForClients[i], lbClientPort, requestRate, simTime);
    clientNodes.Get(i)->AddApplication(app);
    app->SetStartTime(Seconds(1.0));  // small warm-up offset
    app->SetStopTime(Seconds(simTime));
    clientAppPtrs.push_back(app);
  }

  // ---- optional mid-simulation "server failure" (processing slowdown) -
  // Server0's processing rate is cut drastically for a window mid-run to
  // simulate a partial outage / overloaded node, then restored.
  if (injectFailure && nServers > 0) {
    double normalRate = baseServiceRates[0];
    Ptr<BackendServerApp> target = serverAppPtrs[0];
    Simulator::Schedule(Seconds(simTime * 0.35), [target]() {
      target->SetServiceRate(3.0);  // becomes very slow
    });
    Simulator::Schedule(Seconds(simTime * 0.70), [target, normalRate]() {
      target->SetServiceRate(normalRate);  // recovers
    });
  }

  // ---- FlowMonitor ------------------------------------------------
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();

  Simulator::Stop(Seconds(simTime + 1.0));
  Simulator::Run();

  monitor->CheckForLostPackets();
  monitor->SerializeToXmlFile(flowMonFile, true, true);

  // ---- write our own summary CSV (app-level + routing distribution) ---
  std::ofstream csv(summaryFile);
  csv << "metric,value\n";
  csv << "algorithm," << algoStr << "\n";
  csv << "num_servers," << nServers << "\n";
  csv << "num_clients," << nClients << "\n";
  csv << "sim_time_sec," << simTime << "\n";

  uint64_t totalSent = 0, totalRecv = 0;
  double rttSumWeighted = 0;
  for (auto &c : clientAppPtrs) {
    totalSent += c->GetRequestsSent();
    totalRecv += c->GetResponsesReceived();
    rttSumWeighted += c->GetAvgRtt() * c->GetResponsesReceived();
  }
  double overallAvgRtt = totalRecv ? rttSumWeighted / totalRecv : 0.0;
  csv << "total_requests_sent," << totalSent << "\n";
  csv << "total_responses_received," << totalRecv << "\n";
  csv << "packet_loss_pct," << (totalSent ? 100.0 * (totalSent - totalRecv) / totalSent : 0.0) << "\n";
  csv << "avg_app_rtt_sec," << overallAvgRtt << "\n";

  const auto &routed = lbApp->GetRoutedCounts();
  for (size_t i = 0; i < routed.size(); ++i) {
    csv << "server" << i << "_requests_routed," << routed[i] << "\n";
  }
  for (size_t i = 0; i < serverAppPtrs.size(); ++i) {
    csv << "server" << i << "_requests_served," << serverAppPtrs[i]->GetRequestsServed() << "\n";
  }
  csv.close();

  NS_LOG_UNCOND("Done. algo=" << algoStr
                << " sent=" << totalSent << " recv=" << totalRecv
                << " avgRTT=" << overallAvgRtt << "s"
                << " -> " << summaryFile << " / " << flowMonFile);

  Simulator::Destroy();
  return 0;
}
