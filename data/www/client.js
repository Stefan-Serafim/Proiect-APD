// data/www/client.js
const STORAGE_KEY = "apd_offline_chat_history_v1";
export const MAX_PAYLOAD_BYTES = 250; // ESP-NOW v1 payload limit

const encoder = new TextEncoder();

export class ChatClient {
  constructor(ui, options = {}) {
    this.ui = ui;
    const host = window.location.hostname || "192.168.4.1";
    this.wsUrl = options.wsUrl || `ws://${host}/ws`;
    this.apiBaseUrl = options.apiBaseUrl || `http://${host}`;
    this.useMock = options.useMock ?? true;
    this.enableRestFallback = options.enableRestFallback ?? false;
    this.maxPayloadBytes = options.maxPayloadBytes ?? MAX_PAYLOAD_BYTES;

    this.ws = null;
    this.reconnectTimer = null;
    this.reconnectAttempt = 0;
    this.peerOnline = false;
    this.wsConnected = false;
    this.messages = this.deduplicateAndSort(this.loadHistory());
    this.listeners = new Set();
  }

  getByteLength(text = "") {
    return encoder.encode(String(text)).length;
  }

  getConnectionSummary() {
    return {
      mode: this.useMock ? "mock" : "websocket",
      wsUrl: this.wsUrl,
      enableRestFallback: this.enableRestFallback,
      wsConnected: this.wsConnected,
      peerOnline: this.peerOnline,
      reconnectAttempt: this.reconnectAttempt,
      messageCount: this.messages.length,
      maxPayloadBytes: this.maxPayloadBytes,
    };
  }

  onEvent(cb) {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  emit(event) {
    this.listeners.forEach((cb) => cb(event));
  }

  deduplicateAndSort(list) {
    if (!Array.isArray(list)) return [];
    const byId = new Map();
    for (const item of list) {
      if (!item || !item.id) continue;
      const prev = byId.get(item.id);
      byId.set(item.id, prev ? { ...prev, ...item } : item);
    }
    return Array.from(byId.values()).sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
  }

  loadHistory() {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      const parsed = raw ? JSON.parse(raw) : [];
      return Array.isArray(parsed) ? parsed : [];
    } catch {
      return [];
    }
  }

  saveHistory() {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(this.messages));
    } catch (err) {
      console.warn("Eroare la salvarea istoricului în localStorage:", err);
    }
  }

  init() {
    this.ui?.renderMessages?.(this.messages);
    this.ui?.setConnectionState?.({ wsConnected: false, peerOnline: false });

    if (this.useMock) {
      this.startMockMode();
    } else {
      this.connectWebSocket();
      if (this.enableRestFallback) {
        this.fetchHistoryRest();
      }
    }
  }

  setMode({ useMock, wsUrl, apiBaseUrl, enableRestFallback } = {}) {
    if (typeof wsUrl === "string" && wsUrl.trim()) {
      this.wsUrl = wsUrl.trim();
    }
    if (typeof apiBaseUrl === "string" && apiBaseUrl.trim()) {
      this.apiBaseUrl = apiBaseUrl.trim().replace(/\/+$/, "");
    }
    if (typeof useMock === "boolean") {
      this.useMock = useMock;
    }
    if (typeof enableRestFallback === "boolean") {
      this.enableRestFallback = enableRestFallback;
    }

    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }

    if (this.ws) {
      this.ws.onclose = null;
      this.ws.close();
      this.ws = null;
    }

    this.reconnectAttempt = 0;
    this.init();
  }

  startMockMode() {
    this.wsConnected = true;
    this.peerOnline = true;
    this.ui?.setConnectionState?.({ wsConnected: true, peerOnline: true });
    this.emit({ type: "ready", mode: "mock" });
  }

  async fetchHistoryRest() {
    try {
      const res = await fetch(`${this.apiBaseUrl}/api/history`);
      if (!res.ok) return;
      const data = await res.json();
      if (Array.isArray(data?.messages)) {
        this.handleIncomingServerMessage({ type: "history", messages: data.messages });
      }
    } catch {
      // Ignorăm când endpoint-ul REST nu este încă disponibil
    }
  }

  connectWebSocket() {
    if (
      this.ws &&
      (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    try {
      this.ws = new WebSocket(this.wsUrl);
    } catch {
      this.scheduleReconnect();
      return;
    }

    this.ws.onopen = () => {
      this.reconnectAttempt = 0;
      this.wsConnected = true;
      this.ui?.setConnectionState?.({ wsConnected: true, peerOnline: this.peerOnline });
      this.ws.send(JSON.stringify({ type: "sync_request" }));
      this.emit({ type: "ws_open" });
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        this.handleIncomingServerMessage(data);
      } catch (err) {
        console.warn("Cadru JSON invalid primit de la server:", err);
      }
    };

    this.ws.onclose = () => {
      this.wsConnected = false;
      this.ui?.setConnectionState?.({ wsConnected: false, peerOnline: this.peerOnline });
      this.emit({ type: "ws_close" });
      this.scheduleReconnect();
    };

    this.ws.onerror = () => {
      this.ws?.close();
    };
  }

  scheduleReconnect() {
    if (this.useMock || this.reconnectTimer) return;
    const delayMs = Math.min(1000 * Math.pow(2, this.reconnectAttempt), 5000);
    this.reconnectAttempt += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connectWebSocket();
    }, delayMs);
  }

  handleIncomingServerMessage(data) {
    if (!data || !data.type) return;

    if (data.type === "history" && Array.isArray(data.messages)) {
      this.messages = this.deduplicateAndSort([...this.messages, ...data.messages]);
      this.saveHistory();
      this.ui?.renderMessages?.(this.messages);
      return;
    }

    if (data.type === "status" && data.id) {
      this.updateMessageStatus(data.id, data.status, data.extra);
      this.emit({ type: "ack", id: data.id, status: data.status, extra: data.extra });
      return;
    }

    if (data.type === "chat" && data.id && typeof data.text === "string") {
      if (this.messages.some((m) => m.id === data.id)) return;
      const msg = {
        id: data.id,
        text: data.text,
        sender: data.sender || "peer",
        timestamp: data.timestamp || Date.now(),
        status: "delivered",
      };
      this.messages = this.deduplicateAndSort([...this.messages, msg]);
      this.saveHistory();
      this.ui?.upsertMessage?.(msg);
      this.emit({ type: "chat_received", message: msg });
      return;
    }

    if (data.type === "peer_status") {
      this.peerOnline = Boolean(data.online);
      this.ui?.setConnectionState?.({ wsConnected: this.wsConnected, peerOnline: this.peerOnline });
    }
  }

  updateMessageStatus(id, status, extra = {}) {
    const target = this.messages.find((m) => m.id === id);
    if (!target) return;
    target.status = status;
    if (extra.retries !== undefined) target.retries = extra.retries;
    if (extra.rttMs !== undefined) target.rttMs = extra.rttMs;
    if (extra.rssi !== undefined) target.rssi = extra.rssi;
    this.saveHistory();
    this.ui?.upsertMessage?.(target);
  }

  validateText(text) {
    const trimmed = String(text ?? "").trim();
    if (!trimmed) return { ok: false, error: "Mesajul este gol." };
    const byteLength = this.getByteLength(trimmed);
    if (byteLength > this.maxPayloadBytes) {
      return {
        ok: false,
        error: `Mesajul are ${byteLength} octeți UTF-8 (limita este ${this.maxPayloadBytes} octeți).`,
      };
    }
    return { ok: true, text: trimmed, byteLength };
  }

  sendMessage(rawText) {
    const check = this.validateText(rawText);
    if (!check.ok) {
      return { ok: false, error: check.error };
    }

    const id = `msg_${Date.now()}_${Math.random().toString(36).slice(2, 7)}`;
    const msg = {
      id,
      text: check.text,
      sender: "self",
      timestamp: Date.now(),
      status: "sending",
      retries: 0,
      byteLength: check.byteLength,
    };

    this.messages = this.deduplicateAndSort([...this.messages, msg]);
    this.saveHistory();
    this.ui?.upsertMessage?.(msg);

    if (this.useMock) {
      const start = performance.now();
      setTimeout(() => {
        const rttMs = Math.round(performance.now() - start);
        const delivered = this.peerOnline && Math.random() > 0.08;
        const retries = delivered ? 0 : 1;
        const rssi = delivered ? Math.round(-52 - Math.random() * 14) : null;
        this.updateMessageStatus(id, delivered ? "delivered" : "failed", { rttMs, rssi, retries });
        this.emit({
          type: "ack",
          id,
          status: delivered ? "delivered" : "failed",
          extra: { rttMs, rssi, retries },
        });
      }, 25 + Math.random() * 35);
      return { ok: true, id, byteLength: check.byteLength };
    }

    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(
        JSON.stringify({ type: "send", id, text: check.text, timestamp: msg.timestamp })
      );
      return { ok: true, id, byteLength: check.byteLength };
    }

    if (this.enableRestFallback) {
      const start = performance.now();
      fetch(`${this.apiBaseUrl}/api/send`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id, text: check.text, timestamp: msg.timestamp }),
      })
        .then((res) => (res.ok ? res.json() : Promise.reject(new Error(`HTTP ${res.status}`))))
        .then((data) => {
          const rttMs = Math.round(performance.now() - start);
          const status = data?.status || "delivered";
          this.updateMessageStatus(id, status, {
            rttMs: data?.rttMs ?? rttMs,
            rssi: data?.rssi,
            retries: data?.retries ?? 0,
          });
          this.emit({
            type: "ack",
            id,
            status,
            extra: { rttMs: data?.rttMs ?? rttMs, rssi: data?.rssi, retries: data?.retries ?? 0 },
          });
        })
        .catch(() => {
          this.updateMessageStatus(id, "failed");
          this.emit({ type: "ack", id, status: "failed", extra: {} });
        });
      return { ok: true, id, byteLength: check.byteLength };
    }

    this.updateMessageStatus(id, "failed");
    return { ok: false, error: "Conexiunea WebSocket către nodul local nu este activă." };
  }

  clearHistory() {
    this.messages = [];
    this.saveHistory();
    this.ui?.renderMessages?.(this.messages);
  }
}