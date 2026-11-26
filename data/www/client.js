// data/www/client.js
const STORAGE_KEY = "apd_offline_chat_history_v1";
const MAX_PAYLOAD_BYTES = 250; // ESP-NOW v1 payload limit
const encoder = new TextEncoder();

export class ChatClient {
  constructor(ui, options = {}) {
    this.ui = ui;
    this.wsUrl = options.wsUrl || `ws://${window.location.hostname || "192.168.4.1"}/ws`;
    this.useMock = options.useMock ?? true;
    this.ws = null;
    this.reconnectTimer = null;
    this.peerOnline = false;
    this.wsConnected = false;
    this.messages = this.loadHistory();
    this.listeners = new Set();
  }

  onEvent(cb) {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  emit(event) {
    this.listeners.forEach((cb) => cb(event));
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
    if (this.useMock) this.startMockMode();
    else this.connectWebSocket();
  }

  startMockMode() {
    this.wsConnected = true;
    this.peerOnline = true;
    this.ui?.setConnectionState?.({ wsConnected: true, peerOnline: true });
    this.emit({ type: "ready", mode: "mock" });
  }

  connectWebSocket() {
    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) return;
    try {
      this.ws = new WebSocket(this.wsUrl);
    } catch {
      this.scheduleReconnect();
      return;
    }

    this.ws.onopen = () => {
      this.wsConnected = true;
      this.ui?.setConnectionState?.({ wsConnected: true, peerOnline: this.peerOnline });
      this.ws.send(JSON.stringify({ type: "sync_request" }));
    };

    this.ws.onmessage = (event) => {
      try {
        this.handleIncomingServerMessage(JSON.parse(event.data));
      } catch (err) {
        console.warn("Cadru JSON invalid primit de la server:", err);
      }
    };

    this.ws.onclose = () => {
      this.wsConnected = false;
      this.ui?.setConnectionState?.({ wsConnected: false, peerOnline: this.peerOnline });
      this.scheduleReconnect();
    };

    this.ws.onerror = () => this.ws?.close();
  }

  scheduleReconnect() {
    if (this.useMock || this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connectWebSocket();
    }, 2000);
  }

  handleIncomingServerMessage(data) {
    if (!data || !data.type) return;
    if (data.type === "history" && Array.isArray(data.messages)) {
      const byId = new Map(this.messages.map((m) => [m.id, m]));
      for (const m of data.messages) byId.set(m.id, { ...byId.get(m.id), ...m });
      this.messages = Array.from(byId.values()).sort((a, b) => a.timestamp - b.timestamp);
      this.saveHistory();
      this.ui?.renderMessages?.(this.messages);
    } else if (data.type === "status" && data.id) {
      this.updateMessageStatus(data.id, data.status, data.extra);
      this.emit({ type: "ack", id: data.id, status: data.status, extra: data.extra });
    } else if (data.type === "chat" && data.id && typeof data.text === "string") {
      if (this.messages.some((m) => m.id === data.id)) return;
      const msg = {
        id: data.id,
        text: data.text,
        sender: data.sender || "peer",
        timestamp: data.timestamp || Date.now(),
        status: "delivered",
      };
      this.messages.push(msg);
      this.saveHistory();
      this.ui?.upsertMessage?.(msg);
      this.emit({ type: "chat_received", message: msg });
    } else if (data.type === "peer_status") {
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
    this.saveHistory();
    this.ui?.upsertMessage?.(target);
  }

  validateText(text) {
    const trimmed = text.trim();
    if (!trimmed) return { ok: false, error: "Mesajul este gol." };
    const byteLength = encoder.encode(trimmed).length;
    if (byteLength > MAX_PAYLOAD_BYTES) {
      return { ok: false, error: `Mesajul are ${byteLength} octeți UTF-8 (limita este ${MAX_PAYLOAD_BYTES} octeți).` };
    }
    return { ok: true, text: trimmed, byteLength };
  }

  sendMessage(rawText) {
    const check = this.validateText(rawText);
    if (!check.ok) return { ok: false, error: check.error };

    const id = `msg_${Date.now()}_${Math.random().toString(36).slice(2, 7)}`;
    const msg = { id, text: check.text, sender: "self", timestamp: Date.now(), status: "sending", retries: 0 };
    this.messages.push(msg);
    this.saveHistory();
    this.ui?.upsertMessage?.(msg);

    if (this.useMock) {
      const start = performance.now();
      setTimeout(() => {
        const rttMs = Math.round(performance.now() - start);
        const delivered = this.peerOnline && Math.random() > 0.08;
        this.updateMessageStatus(id, delivered ? "delivered" : "failed", { rttMs });
        this.emit({ type: "ack", id, status: delivered ? "delivered" : "failed", extra: { rttMs } });
      }, 25 + Math.random() * 35);
      return { ok: true, id };
    }

    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ type: "send", id, text: check.text, timestamp: msg.timestamp }));
      return { ok: true, id };
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