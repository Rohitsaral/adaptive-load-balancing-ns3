import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

fig, ax = plt.subplots(figsize=(11, 6.5))
ax.set_xlim(0, 11)
ax.set_ylim(0, 6.5)
ax.axis("off")

def box(x, y, w, h, text, color, fontsize=10, textcolor="white"):
    b = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.08",
                        linewidth=1.5, edgecolor=color, facecolor=color, alpha=0.9)
    ax.add_patch(b)
    ax.text(x + w/2, y + h/2, text, ha="center", va="center",
             fontsize=fontsize, color=textcolor, fontweight="bold")

def arrow(x1, y1, x2, y2, color="#555555"):
    a = FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>", mutation_scale=14,
                          linewidth=1.4, color=color)
    ax.add_patch(a)

# Clients (left)
client_positions = [(0.3, 5.3), (0.3, 4.1), (0.3, 2.9), (0.3, 1.7)]
labels = ["Client 0", "Client 1", "Client 2", "...N"]
for (x, y), lab in zip(client_positions, labels):
    box(x, y, 1.6, 0.7, lab, "#4C72B0", fontsize=9)

# Load Balancer (center)
box(4.2, 3.0, 2.6, 1.2, "Load Balancer\n(LoadBalancerApp)", "#C44E52", fontsize=11)
ax.text(5.5, 2.75, "Round Robin | Weighted RR |\nAdaptive (Heuristic) | Q-Learning (ML)",
         ha="center", va="top", fontsize=8, color="#333333", style="italic")

# Servers (right)
server_positions = [(8.1, 5.0), (8.1, 3.4), (8.1, 1.8)]
server_labels = ["Server 0\n(strong)", "Server 1\n(medium)", "Server 2\n(weak)"]
for (x, y), lab in zip(server_positions, server_labels):
    box(x, y, 1.9, 0.9, lab, "#55A868", fontsize=9)

# arrows clients -> LB
for x, y in client_positions:
    arrow(x + 1.6, y + 0.35, 4.2, 3.6)

# arrows LB -> servers
for x, y in server_positions:
    arrow(6.8, 3.6, x, y + 0.45)

ax.text(5.5, 6.0, "Adaptive Load Balancing -- NS-3 Star Topology",
         ha="center", fontsize=14, fontweight="bold")
ax.text(5.5, 0.6,
        "P2P links: 10Mbps/5ms (client\u2194LB), 20Mbps/2ms (LB\u2194server)   |   UDP relay, FlowMonitor attached",
        ha="center", fontsize=8.5, color="#555555")

fig.tight_layout()
fig.savefig("results/architecture_diagram.png", dpi=150, bbox_inches="tight")
print("saved")
