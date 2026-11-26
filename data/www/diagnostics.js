// data/www/diagnostics.js
export class DiagnosticsRunner {
  constructor(client, ui) {
    this.client = client;
    this.ui = ui;
    this.records = [];
    this.running = false;
    this.abortRequested = false;
    this.lastScenario = "default";
  }

  stop() {
    if (this.running) {
      this.abortRequested = true;
    }
  }

  reset() {
    if (this.running) return;
    this.records = [];
    this.ui?.renderDiagnostics?.(this.getSummary(), this.records);
  }

  async runBurst({
    count = 20,
    intervalMs = 120,
    timeoutMs = 1500,
    scenario = "default",
    append = false,
  } = {}) {
    if (this.running) return this.getSummary();
    this.running = true;
    this.abortRequested = false;
    this.lastScenario = String(scenario || "default").trim() || "default";

    if (!append) {
      this.records = [];
    }

    const startSeq = this.records.length;
    this.ui?.renderDiagnostics?.(this.getSummary(), this.records);

    for (let i = 1; i <= count; i++) {
      if (this.abortRequested) break;

      const seq = startSeq + i;
      const t0 = performance.now();
      const probeText = `[DIAG #${seq} (${this.lastScenario})]`;
      const res = this.client.sendMessage(probeText);

      if (!res.ok) {
        this.records.push({
          seq,
          scenario: this.lastScenario,
          id: "",
          status: "failed",
          retries: 0,
          rttMs: null,
          rssi: null,
          error: res.error || "Send failed",
          timestamp: new Date().toISOString(),
        });
      } else {
        const outcome = await this.waitForAck(res.id, timeoutMs);
        const rttMs =
          typeof outcome?.extra?.rttMs === "number"
            ? outcome.extra.rttMs
            : Math.round(performance.now() - t0);
        const delivered = outcome?.status === "delivered";

        this.records.push({
          seq,
          scenario: this.lastScenario,
          id: res.id,
          status: delivered ? "delivered" : "failed",
          retries: outcome?.extra?.retries ?? 0,
          rttMs: delivered ? rttMs : null,
          rssi: outcome?.extra?.rssi ?? (this.client.useMock ? -58 : null),
          error: delivered ? "" : "Timeout / No ACK",
          timestamp: new Date().toISOString(),
        });
      }

      this.ui?.renderDiagnostics?.(this.getSummary(), this.records);

      if (i < count && intervalMs > 0 && !this.abortRequested) {
        await new Promise((r) => setTimeout(r, intervalMs));
      }
    }

    this.running = false;
    this.abortRequested = false;
    const summary = this.getSummary();
    this.ui?.renderDiagnostics?.(summary, this.records);
    return summary;
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

  percentile(sortedValues, p) {
    if (!sortedValues.length) return 0;
    const idx = Math.ceil((p / 100) * sortedValues.length) - 1;
    return Number(sortedValues[Math.max(0, Math.min(sortedValues.length - 1, idx))].toFixed(2));
  }

  getSummary() {
    const sent = this.records.length;
    const deliveredRows = this.records.filter(
      (r) => r.status === "delivered" && typeof r.rttMs === "number"
    );
    const delivered = deliveredRows.length;
    const firstTryDelivered = deliveredRows.filter((r) => (r.retries ?? 0) === 0).length;
    const retriedCount = this.records.filter((r) => (r.retries ?? 0) > 0).length;
    const failed = sent - delivered;
    const packetLossPct = sent > 0 ? Number((((sent - delivered) / sent) * 100).toFixed(2)) : 0;

    const rtts = deliveredRows.map((r) => r.rttMs).sort((a, b) => a - b);
    const minRttMs = rtts.length > 0 ? Number(rtts[0].toFixed(2)) : 0;
    const maxRttMs = rtts.length > 0 ? Number(rtts[rtts.length - 1].toFixed(2)) : 0;
    const avgRttMs =
      rtts.length > 0
        ? Number((rtts.reduce((acc, v) => acc + v, 0) / rtts.length).toFixed(2))
        : 0;
    const medianRttMs = this.percentile(rtts, 50);
    const p95RttMs = this.percentile(rtts, 95);

    const stdDevRttMs =
      rtts.length > 1
        ? Number(
            Math.sqrt(
              rtts.reduce((acc, v) => acc + Math.pow(v - avgRttMs, 2), 0) / rtts.length
            ).toFixed(2)
          )
        : 0;

    const rssiValues = this.records
      .map((r) => r.rssi)
      .filter((v) => typeof v === "number" && !Number.isNaN(v));
    const avgRssi =
      rssiValues.length > 0
        ? Number((rssiValues.reduce((acc, v) => acc + v, 0) / rssiValues.length).toFixed(2))
        : null;

    return {
      scenario: this.lastScenario,
      running: this.running,
      sent,
      delivered,
      firstTryDelivered,
      retriedCount,
      failed,
      packetLossPct,
      minRttMs,
      maxRttMs,
      avgRttMs,
      stdDevRttMs,
      medianRttMs,
      p95RttMs,
      avgRssi,
    };
  }

  downloadCsv(csvContent, filename) {
    const blob = new Blob([csvContent], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.setAttribute("download", filename);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
  }

  exportCsv(filename = "apd_diag_results.csv") {
    const headers = [
      "seq",
      "scenario",
      "id",
      "status",
      "retries",
      "rtt_ms",
      "rssi",
      "error",
      "timestamp",
    ];
    const lines = [
      headers.join(","),
      ...this.records.map((r) =>
        [
          r.seq,
          r.scenario ?? this.lastScenario,
          r.id,
          r.status,
          r.retries ?? 0,
          r.rttMs ?? "",
          r.rssi ?? "",
          r.error ?? "",
          r.timestamp,
        ]
          .map((val) => `"${String(val).replaceAll('"', '""')}"`)
          .join(",")
      ),
    ];
    this.downloadCsv(lines.join("\n"), filename);
  }

  exportSummaryCsv(filename = "apd_diag_summary.csv") {
    const s = this.getSummary();
    const headers = [
      "scenario",
      "sent",
      "delivered",
      "first_try_delivered",
      "retried",
      "failed",
      "packet_loss_pct",
      "min_rtt_ms",
      "max_rtt_ms",
      "avg_rtt_ms",
      "stddev_rtt_ms",
      "median_rtt_ms",
      "p95_rtt_ms",
      "avg_rssi",
    ];
    const row = [
      s.scenario,
      s.sent,
      s.delivered,
      s.firstTryDelivered,
      s.retriedCount,
      s.failed,
      s.packetLossPct,
      s.minRttMs,
      s.maxRttMs,
      s.avgRttMs,
      s.stdDevRttMs,
      s.medianRttMs,
      s.p95RttMs,
      s.avgRssi ?? "",
    ]
      .map((val) => `"${String(val).replaceAll('"', '""')}"`)
      .join(",");

    this.downloadCsv([headers.join(","), row].join("\n"), filename);
  }
}