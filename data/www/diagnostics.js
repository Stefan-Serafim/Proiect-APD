// data/www/diagnostics.js
export class DiagnosticsRunner {
  constructor(client, ui) {
    this.client = client;
    this.ui = ui;
    this.records = [];
    this.running = false;
  }

  async runBurst({ count = 20, intervalMs = 120 } = {}) {
    if (this.running) return;
    this.running = true;
    this.records = [];

    for (let i = 1; i <= count; i++) {
      const t0 = performance.now();
      const probeText = `[DIAG #${i}/${count}]`;
      const res = this.client.sendMessage(probeText);

      if (!res.ok) {
        this.records.push({
          seq: i,
          id: "",
          status: "failed",
          rttMs: null,
          rssi: null,
          timestamp: new Date().toISOString(),
        });
      } else {
        const outcome = await this.waitForAck(res.id, 1500);
        const rttMs = outcome?.extra?.rttMs ?? Math.round(performance.now() - t0);
        this.records.push({
          seq: i,
          id: res.id,
          status: outcome?.status || "failed",
          rttMs: outcome?.status === "delivered" ? rttMs : null,
          rssi: outcome?.extra?.rssi ?? (this.client.useMock ? -58 : null),
          timestamp: new Date().toISOString(),
        });
      }

      this.ui?.renderDiagnostics?.(this.getSummary(), this.records);
      if (i < count && intervalMs > 0) {
        await new Promise((r) => setTimeout(r, intervalMs));
      }
    }

    this.running = false;
    this.ui?.renderDiagnostics?.(this.getSummary(), this.records);
    return this.getSummary();
  }

  waitForAck(msgId, timeoutMs) {
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        unsub();
        resolve({ id: msgId, status: "failed", extra: {} });
      }, timeoutMs);

      const unsub = this.client.onEvent((ev) => {
        if (ev.type === "ack" && ev.id === msgId) {
          clearTimeout(timer);
          unsub();
          resolve(ev);
        }
      });
    });
  }

  getSummary() {
    const sent = this.records.length;
    const deliveredRows = this.records.filter((r) => r.status === "delivered" && typeof r.rttMs === "number");
    const delivered = deliveredRows.length;
    const failed = sent - delivered;
    const packetLossPct = sent > 0 ? Number((((sent - delivered) / sent) * 100).toFixed(2)) : 0;
    const avgRttMs =
      delivered > 0
        ? Number((deliveredRows.reduce((acc, r) => acc + r.rttMs, 0) / delivered).toFixed(2))
        : 0;

    return { sent, delivered, failed, packetLossPct, avgRttMs };
  }

  exportCsv(filename = "apd_diag_results.csv") {
    const headers = ["seq", "id", "status", "rtt_ms", "rssi", "timestamp"];
    const lines = [
      headers.join(","),
      ...this.records.map((r) =>
        [r.seq, r.id, r.status, r.rttMs ?? "", r.rssi ?? "", r.timestamp]
          .map((val) => `"${String(val).replaceAll('"', '""')}"`)
          .join(",")
      ),
    ];

    const blob = new Blob([lines.join("\n")], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.setAttribute("download", filename);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
  }
}