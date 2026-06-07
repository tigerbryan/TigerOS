const $ = (id) => document.getElementById(id);
const TOKEN_KEY = "tigeros_api_token";
const LANG_KEY = "tigeros_language";
const GATT_SNAPSHOTS_KEY = "tigeros_gatt_snapshots";
let apiToken = localStorage.getItem(TOKEN_KEY) || "";
let currentLanguage = localStorage.getItem(LANG_KEY) || "en";
const LOCATIONS = ["Fridge", "Freezer", "Greenhouse", "Cat Room", "Reptile Box", "Custom"];
let pairedDevicesCache = [];
let discoveredDevicesCache = [];
let bleRawPacketsCache = [];
let bleStatsCache = {};
let logsCache = [];
let diagnosticsCache = "";
let discoveredVisibleLimit = 10;
let deviceRawCache = {};
let drawerDevice = null;
let bleGattInspectionCache = null;
let gattSnapshotsCache = [];
let latestStatusUptime = 0;
let webhookFormDirty = false;
let onboardingActive = window.location.hostname === "192.168.4.1";

function t(key) {
  return window.TIGEROS_I18N?.[currentLanguage]?.[key] || window.TIGEROS_I18N?.en?.[key] || key;
}

function applyI18n() {
  document.documentElement.lang = currentLanguage;
  document.querySelectorAll("[data-i18n]").forEach((element) => {
    const key = element.dataset.i18n;
    if (key) element.textContent = t(key);
  });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((element) => {
    const key = element.dataset.i18nPlaceholder;
    if (key) element.placeholder = t(key);
  });
  $("language-select").value = currentLanguage;
}

function formatUptime(seconds) {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m ${seconds % 60}s`;
}

function setText(id, value) {
  $(id).textContent = value || "-";
}

function setStatus(id, message, type = "") {
  const element = $(id);
  element.textContent = message;
  element.classList.toggle("success", type === "success");
  element.classList.toggle("error", type === "error");
}

function showToast(message, type = "success") {
  const toast = $("toast");
  toast.textContent = message;
  toast.className = `toast ${type}`;
  toast.hidden = false;
  setTimeout(() => {
    toast.hidden = true;
  }, 7000);
}

function switchTab(name) {
  document.querySelectorAll("[data-tab]").forEach((button) => {
    const active = button.dataset.tab === name;
    button.classList.toggle("active", active);
    button.setAttribute("aria-selected", active ? "true" : "false");
  });
  document.querySelectorAll("[data-tab-panel]").forEach((panel) => {
    const active = panel.dataset.tabPanel === name;
    panel.hidden = !active;
    panel.classList.toggle("active", active);
  });
}

function escapeText(value) {
  return String(value || "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  }[char]));
}

function authHeaders(extra = {}) {
  return apiToken ? { ...extra, Authorization: `Bearer ${apiToken}` } : extra;
}

async function apiFetch(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    headers: authHeaders(options.headers || {}),
  });
  const text = await response.text();
  let data = {};
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    data = response.ok ? { ok: true, message: text || `HTTP ${response.status}` } : { ok: false, error: text || `HTTP ${response.status}` };
  }
  if (!response.ok || !data.ok) {
    const fallback = response.ok ? "Request failed" : `HTTP ${response.status} ${response.statusText || ""}`.trim();
    const message = data.error || data.message || fallback;
    throw new Error(`${message} (${url})`);
  }
  return data;
}

function showAppView(view) {
  $("onboarding-view").hidden = view !== "onboarding";
  $("login-view").hidden = view !== "login";
  $("dashboard-view").hidden = view !== "dashboard";
  $("logout-button").hidden = view !== "dashboard";
}

function showDashboard(show) {
  showAppView(show ? "dashboard" : "login");
}

function updateOnboardingState(status) {
  const setupMode = !status.wifi_connected && status.wifi_mode === "setup_ap";
  const onSetupAddress = window.location.hostname === "192.168.4.1";
  if (setupMode || (onboardingActive && onSetupAddress && !apiToken)) {
    showAppView("onboarding");
  } else if (!apiToken) {
    showAppView("login");
  }

  if ($("onboarding-ap")) {
    $("onboarding-ap").textContent = status.ap_ssid || "TigerOS-Setup";
  }

  const connectedBox = $("onboarding-connected");
  const lanLink = $("onboarding-lan-link");
  if (status.wifi_connected && status.ip_address && status.ip_address !== "192.168.4.1") {
    onboardingActive = true;
    connectedBox.hidden = false;
    lanLink.href = `http://${status.ip_address}`;
    lanLink.textContent = `http://${status.ip_address}`;
    setStatus("onboarding-status", `${t("connectedLanIp")}: ${status.ip_address}`, "success");
  } else if (!status.wifi_connected && connectedBox) {
    connectedBox.hidden = true;
  }
}

function setOtaFile(file) {
  if (!file) return;
  if (!file.name.toLowerCase().endsWith(".bin")) {
    setStatus("ota-status", "Only ESP-IDF .bin firmware files are supported.", "error");
    showToast("Please choose a .bin firmware file.", "error");
    return;
  }

  const input = $("ota-file");
  const transfer = new DataTransfer();
  transfer.items.add(file);
  input.files = transfer.files;
  $("ota-file-name").textContent = `${file.name} (${Math.round(file.size / 1024)} KB)`;
  setStatus("ota-status", "Firmware file ready to upload.");
}

async function loadStatus() {
  const response = await fetch("/api/status", {
    cache: "no-store",
    headers: authHeaders(),
  });
  const data = await response.json();

  setText("device-id", data.device_id || (apiToken ? "-" : "Login required"));
  setText("firmware-version", data.firmware_version);
  setText("update-firmware-version", data.firmware_version);
  setText("build-time", data.build_time);
  setText("wifi-status", data.wifi_connected ? `Connected to ${data.wifi_ssid || "WiFi"}` : data.wifi_mode);
  setText("ip-address", data.ip_address);
  setText("uptime", formatUptime(Number(data.uptime_seconds || 0)));
  setText("free-heap", `${data.free_heap} bytes`);
  latestStatusUptime = Number(data.uptime_seconds || 0);

  const pill = $("status-pill");
  pill.textContent = data.wifi_connected ? t("online") : t("setupMode");
  pill.classList.toggle("online", Boolean(data.wifi_connected));

  updateOnboardingState(data);
}

async function loadMqtt() {
  const data = await apiFetch("/api/mqtt", { cache: "no-store" });
  $("mqtt-enabled").checked = Boolean(data.enabled);
  $("mqtt-host").value = data.host || "";
  $("mqtt-port").value = data.port || (data.use_tls ? 8883 : 1883);
  $("mqtt-username").value = data.username || "";
  $("mqtt-password").value = "";
  $("mqtt-password").placeholder = data.password_set ? "Password saved; leave blank to keep it" : "Optional password";
  $("mqtt-client-id").value = data.client_id || "";
  $("mqtt-use-tls").checked = Boolean(data.use_tls);
  $("ha-discovery-enabled").checked = data.ha_discovery_enabled !== false;
  $("ha-discovery-prefix").value = data.ha_discovery_prefix || "homeassistant";
  setText("mqtt-status", data.state || "Disabled");
  setStatus("mqtt-form-status", data.password_set ? "MQTT password is saved in NVS and hidden here." : "");
}

async function loadWebhook() {
  if (webhookFormDirty && $("webhook-form")?.contains(document.activeElement)) {
    return;
  }
  const data = await apiFetch("/api/webhook", { cache: "no-store" });
  $("webhook-enabled").checked = Boolean(data.enabled);
  $("webhook-url").value = data.url || "";
  $("webhook-secret-header").value = data.secret_header || "x-ingest-secret";
  $("webhook-secret-value").value = "";
  $("webhook-secret-value").placeholder = data.secret_set ? "Secret saved; leave blank to keep it" : "Paste webhook secret";
  $("webhook-interval").value = data.interval_seconds || 300;
  $("webhook-status").textContent = data.enabled ? t("enabled") : "Disabled";
  setStatus("webhook-form-status", data.secret_set ? t("webhookSecretSaved") : "");
}

async function loadBle() {
  const data = await apiFetch("/api/ble", { cache: "no-store" });
  $("ble-enabled").checked = Boolean(data.enabled);
  $("ble-pairing-pin").value = data.pairing_pin || "";
  $("ble-pop-token").value = "";
  $("ble-device-name").textContent = data.device_name || "-";
  $("ble-provisioning-state").textContent = data.provisioning_state || "-";
  $("ble-pop-required").textContent = data.pop_required ? "Required" : "Disabled";
  setText("ble-status", data.enabled ? data.state : "Disabled");
  setStatus("ble-form-status", data.pop_required ? "PoP token is enabled and hidden here." : "");
}

async function loadHardware() {
  const data = await apiFetch("/api/hardware", { cache: "no-store" });
  $("hardware-profile").textContent = data.profile || "-";
  $("hardware-display").textContent = data.display_available
    ? `${data.display_driver || "TFT"} ${data.display_width || "-"}x${data.display_height || "-"}`
    : t("unavailable");
  $("hardware-audio").textContent = data.audio_available ? (data.audio_enabled ? t("enabled") : t("driverPending")) : t("unavailable");
  $("display-enabled").checked = Boolean(data.display_enabled);
  $("display-backlight").checked = Boolean(data.display_backlight_on);
  $("display-meta").textContent = `${data.display_driver || "TFT"} ${data.display_width || "-"}x${data.display_height || "-"} · BL GPIO ${data.backlight_gpio ?? "-"}`;
  $("audio-meta").textContent = data.audio_available ? `${t("speaker")} · ${data.audio_mode || t("driverPending")}` : t("unavailable");
  $("camera-meta").textContent = data.camera_available ? t("cameraHardwareAvailable") : t("unavailable");
  $("microphone-meta").textContent = data.microphone_available ? t("microphoneHardwareAvailable") : t("unavailable");
  $("hardware-note").textContent = data.note || "";
}

function sensorValue(sensor) {
  const parts = [];
  if (sensor.temperature_c !== undefined) parts.push(`${t("temperature")} ${Number(sensor.temperature_c).toFixed(2)} C`);
  if (sensor.external_probe_temperature_c !== undefined) parts.push(`probe ${Number(sensor.external_probe_temperature_c).toFixed(2)} C`);
  if (sensor.humidity_percent !== undefined) parts.push(`${t("humidity")} ${Number(sensor.humidity_percent).toFixed(2)}%`);
  if (sensor.battery_percent !== undefined) parts.push(`${t("battery")} ${sensor.battery_percent}%`);
  return parts.join(" / ") || "Raw packet captured";
}

function deviceStateSummary(device) {
  const state = device.state || {};
  const parts = [];
  if (state.temperature_c !== undefined) parts.push(`${t("temperature")} ${Number(state.temperature_c).toFixed(2)} C`);
  if (state.external_probe_temperature_c !== undefined) parts.push(`${t("externalProbe")} ${Number(state.external_probe_temperature_c).toFixed(2)} C`);
  if (state.humidity_percent !== undefined) parts.push(`${t("humidity")} ${Number(state.humidity_percent).toFixed(2)}%`);
  if (state.battery_percent !== undefined) parts.push(`${t("battery")} ${state.battery_percent}%`);
  if (state.power !== undefined) parts.push(`${t("power")} ${state.power ? t("on") : t("off")}`);
  if (!parts.length) {
    const keys = Object.keys(state);
    return keys.length ? keys.slice(0, 4).map((key) => `${key}: ${state[key]}`).join(" / ") : t("noState");
  }
  return parts.join(" / ");
}

function formatLastSeen(seconds) {
  const value = Number(seconds || 0);
  if (value <= 0) return "-";
  if (value < 60) return `${value}s`;
  return formatUptime(value);
}

function formatSeenAge(lastSeen) {
  const seen = Number(lastSeen || 0);
  if (seen <= 0 || latestStatusUptime <= 0) return "-";
  const age = Math.max(0, latestStatusUptime - seen);
  if (age < 60) return `${age}s ${t("ago")}`;
  return `${formatUptime(age)} ${t("ago")}`;
}

function deviceHealth(device) {
  const raw = device.raw || {};
  const parseStatus = String(raw.parse_status || device.parse_status || "").toLowerCase();
  const hasState = readableStateEntries(device).length > 0;
  if (device.online && hasState) {
    return { label: t("online"), detail: `${t("lastSeen")}: ${formatSeenAge(device.last_seen)}`, cls: "is-online" };
  }
  if (device.online) {
    return { label: t("waitingValues"), detail: raw.debug || t("heardNoValues"), cls: "is-waiting" };
  }
  if (parseStatus === "partial") {
    return { label: t("waitingValues"), detail: raw.debug || t("waitingManufacturerPacket"), cls: "is-waiting" };
  }
  if (Number(device.last_seen || 0) > 0) {
    return { label: t("recentlyNotHeard"), detail: `${t("lastSeen")}: ${formatSeenAge(device.last_seen)}`, cls: "is-stale" };
  }
  return { label: t("waitingBroadcast"), detail: t("moveDeviceCloserHint"), cls: "is-offline" };
}

function readableStateEntries(device) {
  const state = device.state || {};
  const preferred = [
    ["temperature_c", t("temperature"), " C"],
    ["external_probe_temperature_c", "Probe", " C"],
    ["humidity_percent", t("humidity"), "%"],
    ["battery_percent", t("battery"), "%"],
    ["power", t("power"), ""],
  ];
  const entries = [];
  const seen = new Set();
  for (const [key, label, suffix] of preferred) {
    if (state[key] === undefined || state[key] === null) continue;
    seen.add(key);
    const value = typeof state[key] === "boolean" ? (state[key] ? t("on") : t("off")) : `${state[key]}${suffix}`;
    entries.push({ key, label, value });
  }
  for (const key of Object.keys(state)) {
    if (seen.has(key)) continue;
    entries.push({ key, label: key, value: String(state[key]) });
  }
  return entries;
}

function stateFromBlePacket(packet) {
  const state = {};
  if (packet.temperature_c !== undefined) state.temperature_c = packet.temperature_c;
  if (packet.humidity_percent !== undefined) state.humidity_percent = packet.humidity_percent;
  if (packet.battery_percent !== undefined) state.battery_percent = packet.battery_percent;
  if (packet.external_probe_temperature_c !== undefined) state.external_probe_temperature_c = packet.external_probe_temperature_c;
  return state;
}

function rawFromBlePacket(packet) {
  return {
    ble_name: packet.name || packet.sensor_name || "",
    parser_protocol: packet.protocol || packet.sensor_type || "",
    parse_status: packet.parse_status || "",
    raw_adv_hex: packet.raw_adv_hex || packet.raw_advertisement || "",
    debug: packet.debug || "",
    rssi: packet.rssi,
    last_seen: packet.last_seen,
  };
}

function renderDeviceDataCard(device, rawResponse = {}) {
  const raw = rawResponse.raw || device.raw || {};
  const entries = readableStateEntries(device);
  const lastSeen = device.last_seen || rawResponse.last_seen || raw.last_seen || 0;
  const payload = {
    schema: "tigeros.device_state.v1",
    gateway_id: $("device-id")?.textContent || "",
    device_id: device.id,
    name: deviceDisplayName(device),
    type: device.type || "sensor",
    brand: device.brand || "generic",
    model: device.model || "unknown",
    protocol: raw.parser_protocol || device.protocol || "unknown",
    address: device.address || device.id,
    location: device.location || "",
    online: Boolean(device.online),
    state: device.state || {},
    rssi: raw.rssi ?? null,
    collected_at_uptime: Number(lastSeen || 0),
    sent_at_uptime: latestStatusUptime,
  };
  const card = document.createElement("article");
  card.className = "data-card";
  card.innerHTML = `
    <div class="data-card-head">
      <div>
        <strong>${escapeText(deviceDisplayName(device))}</strong>
        <code>${escapeText(device.address || device.id)}</code>
      </div>
      <span class="status-pill ${device.online ? "is-online" : "is-offline"}">${device.online ? t("online") : t("offline")}</span>
    </div>
    <div class="data-meta">
      <span>${escapeText(device.brand || "generic")} ${escapeText(device.model || "")}</span>
      <span>${t("protocol")}: ${escapeText(raw.parser_protocol || device.protocol || "unknown")}</span>
      <span>${t("rssi")}: ${raw.rssi ?? "-"} dBm</span>
      <span>${t("collectedAt")}: ${formatSeenAge(lastSeen)}</span>
      <span>${t("sentAt")}: ${formatLastSeen(latestStatusUptime)}</span>
      <span>${t("parserStatus")}: ${escapeText(raw.parse_status || "-")}</span>
    </div>
  `;

  const metrics = document.createElement("div");
  metrics.className = "data-metrics";
  if (entries.length) {
    for (const item of entries) {
      const metric = document.createElement("div");
      metric.className = "data-metric";
      metric.innerHTML = `<span>${escapeText(item.label)}</span><strong>${escapeText(item.value)}</strong>`;
      metrics.appendChild(metric);
    }
  } else {
    const rawOnly = document.createElement("div");
    rawOnly.className = "data-metric raw-only";
    rawOnly.innerHTML = `<span>${t("readableData")}</span><strong>${t("rawOnly")}</strong>`;
    metrics.appendChild(rawOnly);
  }
  card.appendChild(metrics);

  const caps = document.createElement("p");
  caps.className = "sensor-meta";
  caps.textContent = `${t("capabilities")}: ${(device.capabilities || []).join(", ") || "-"}`;
  card.appendChild(caps);

  const rawDetails = document.createElement("details");
  rawDetails.className = "raw-details";
  rawDetails.innerHTML = `
    <summary>${t("rawData")}</summary>
    <div class="raw-stack">
      <label>
        <span>${t("advertisement")}</span>
        <textarea readonly>${escapeText(raw.raw_adv_hex || raw.raw_advertisement || "")}</textarea>
      </label>
      <label>
        <span>${t("decodedData")}</span>
        <textarea readonly>${escapeText(JSON.stringify({
          supermarcat_payload: payload,
          raw,
        }, null, 2))}</textarea>
      </label>
    </div>
  `;
  card.appendChild(rawDetails);
  return card;
}

async function loadBleRawPackets() {
  try {
    const data = await apiFetch("/api/ble/raw", { cache: "no-store" });
    bleRawPacketsCache = data.packets || [];
  } catch {
    bleRawPacketsCache = [];
  }
  return bleRawPacketsCache;
}

async function loadBleStats() {
  try {
    bleStatsCache = await apiFetch("/api/ble/stats", { cache: "no-store" });
  } catch {
    bleStatsCache = {};
  }
  return bleStatsCache;
}

function renderBleRawPacketTable() {
  const table = $("ble-raw-packet-table");
  if (!table) return;
  const packets = bleRawPacketsCache.slice(0, 30);
  if (!packets.length) {
    table.innerHTML = `<tr><td colspan="8">${t("noBlePackets")}</td></tr>`;
    return;
  }
  table.innerHTML = packets.map((packet) => {
    const state = stateFromBlePacket(packet);
    const value = Object.keys(state).length
      ? Object.entries(state).map(([key, val]) => `${key}=${val}`).join(" / ")
      : t("rawOnly");
    const service = packet.service_data_hex || packet.manufacturer_data_hex || packet.raw_adv_hex || packet.raw_advertisement || "-";
    return `
      <tr>
        <td>${formatLastSeen(packet.last_seen)}</td>
        <td><code>${escapeText(packet.mac || packet.sensor_mac || "-")}</code></td>
        <td>${escapeText(packet.name || packet.sensor_name || "-")}</td>
        <td>${packet.rssi ?? "-"} dBm</td>
        <td>${escapeText(packet.protocol || packet.sensor_type || "-")}</td>
        <td>${escapeText(packet.parse_status || "-")}</td>
        <td>${escapeText(value)}</td>
        <td><code>${escapeText(service.slice(0, 72))}${service.length > 72 ? "..." : ""}</code></td>
      </tr>
    `;
  }).join("");
}

function renderBleDiagnostics() {
  const lines = [];
  lines.push(`Raw BLE packets: ${bleRawPacketsCache.length}`);
  lines.push(`Scanner: ${bleStatsCache.scanning ? "scanning" : "idle"} provisioning_adv=${bleStatsCache.provisioning_advertising ? "yes" : "no"} last_packet=${bleStatsCache.last_packet_seen || 0}s`);
  for (const packet of bleRawPacketsCache) {
    const state = [];
    if (packet.temperature_c !== undefined) state.push(`temperature_c=${packet.temperature_c}`);
    if (packet.humidity_percent !== undefined) state.push(`humidity_percent=${packet.humidity_percent}`);
    if (packet.battery_percent !== undefined) state.push(`battery_percent=${packet.battery_percent}`);
    lines.push([
      `packet mac=${packet.mac || packet.sensor_mac || "-"}`,
      `name=${packet.name || "-"}`,
      `rssi=${packet.rssi ?? "-"}`,
      `protocol=${packet.protocol || packet.sensor_type || "-"}`,
      `status=${packet.parse_status || "-"}`,
      state.join(" ") || "state=n/a",
      `raw=${packet.raw_adv_hex || packet.raw_advertisement || "-"}`,
      packet.debug ? `debug=${packet.debug}` : "",
    ].filter(Boolean).join(" | "));
  }

  const bleLogs = logsCache.filter((item) => String(item.tag || "").toLowerCase().includes("ble"));
  lines.push("");
  lines.push(`BLE logs: ${bleLogs.length}`);
  for (const item of bleLogs.slice(-80)) {
    lines.push(`[${item.uptime_seconds}s] ${item.level} ${item.tag}: ${item.message}`);
  }
  $("device-data-diagnostics").textContent = lines.join("\n") || t("noBleDiagnostics");
}

function filteredLogs(filter) {
  if (!filter || filter === "all") return logsCache;
  const needle = filter.toLowerCase();
  return logsCache.filter((item) => `${item.tag || ""} ${item.message || ""}`.toLowerCase().includes(needle));
}

function renderSystemDiagnostics() {
  const filter = $("diagnostics-filter")?.value || "all";
  const lines = [];
  lines.push("=== TigerOS System Diagnostics ===");
  lines.push(`generated_uptime=${$("uptime")?.textContent || "-"}`);
  lines.push(`device_id=${$("device-id")?.textContent || "-"}`);
  lines.push(`firmware=${$("firmware-version")?.textContent || "-"}`);
  lines.push(`ip=${$("ip-address")?.textContent || "-"}`);
  lines.push(`wifi=${$("wifi-status")?.textContent || "-"}`);
  lines.push(`free_heap=${$("free-heap")?.textContent || "-"}`);
  if (window.lastStatus?.board) {
    lines.push(`board_profile=${window.lastStatus.board.profile || "-"}`);
    lines.push(`board_display=${window.lastStatus.board.display_available ? "available" : "unavailable"}/${window.lastStatus.board.display_enabled ? "enabled" : "disabled"}`);
    lines.push(`board_audio=${window.lastStatus.board.audio_available ? "available" : "unavailable"}/${window.lastStatus.board.audio_enabled ? "enabled" : "disabled"}`);
    lines.push(`board_note=${window.lastStatus.board.note || "-"}`);
  }
  lines.push("");
  lines.push(`=== BLE Raw Packets (${bleRawPacketsCache.length}) ===`);
  for (const packet of bleRawPacketsCache) {
    const state = stateFromBlePacket(packet);
    lines.push(JSON.stringify({
      mac: packet.mac || packet.sensor_mac,
      name: packet.name || packet.sensor_name,
      rssi: packet.rssi,
      protocol: packet.protocol || packet.sensor_type,
      parse_status: packet.parse_status,
      state,
      raw_adv_hex: packet.raw_adv_hex || packet.raw_advertisement,
      debug: packet.debug,
    }));
  }
  lines.push("");
  lines.push(`=== Logs filter=${filter} count=${filteredLogs(filter).length} ===`);
  for (const item of filteredLogs(filter).slice(-180)) {
    lines.push(`[${item.uptime_seconds}s] ${item.level} ${item.tag}: ${item.message}`);
  }
  diagnosticsCache = lines.join("\n");
  $("diagnostics-output").textContent = diagnosticsCache || t("noDiagnostics");
}

async function copyTextWithFallback(text, sourceElement) {
  if (navigator.clipboard?.writeText && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return "clipboard";
  }

  const textarea = document.createElement("textarea");
  textarea.value = text;
  textarea.setAttribute("readonly", "");
  textarea.style.position = "fixed";
  textarea.style.left = "-9999px";
  textarea.style.top = "0";
  document.body.appendChild(textarea);
  textarea.focus();
  textarea.select();
  textarea.setSelectionRange(0, textarea.value.length);
  let copied = false;
  try {
    copied = document.execCommand("copy");
  } finally {
    document.body.removeChild(textarea);
  }
  if (copied) return "fallback";

  const selection = window.getSelection();
  const range = document.createRange();
  range.selectNodeContents(sourceElement);
  selection.removeAllRanges();
  selection.addRange(range);
  throw new Error("manual");
}

async function loadSystemDiagnostics() {
  const [status, scan, mqtt] = await Promise.all([
    fetch("/api/status", { cache: "no-store", headers: authHeaders() }).then((response) => response.json()).catch(() => ({})),
    apiFetch("/api/ble-sensors/scan/status", { cache: "no-store" }).catch(() => ({})),
    apiFetch("/api/mqtt", { cache: "no-store" }).catch(() => ({})),
    loadBleStats(),
    loadBleRawPackets(),
    loadLogs(),
  ]);
  window.lastStatus = status;
  $("diag-wifi").textContent = status.wifi_connected ? `${t("online")} ${status.wifi_ssid || ""}` : (status.wifi_mode || "-");
  $("diag-mqtt").textContent = mqtt.state || (mqtt.enabled ? "Enabled" : "Disabled");
  $("diag-ble-scan").textContent = scan.scanning ? t("scanning") : t("idle");
  $("diag-heap").textContent = status.free_heap ? `${status.free_heap} bytes` : "-";
  $("diag-uptime").textContent = formatUptime(Number(status.uptime_seconds || 0));
  $("diag-raw-packets").textContent = bleRawPacketsCache.length;
  renderSystemDiagnostics();
}

async function loadBleDiagnostics() {
  await Promise.all([
    loadBleStats(),
    loadBleRawPackets(),
    loadLogs(),
  ]);
  renderBleDiagnostics();
}

async function loadDeviceData() {
  if (!pairedDevicesCache.length) {
    await loadDevices();
  }
  await Promise.all([loadBleStats(), loadBleRawPackets()]);
  const list = $("device-data-list");
  list.innerHTML = "";
  deviceRawCache = {};
  let onlineCount = 0;
  let readableCount = 0;
  let rawOnlyCount = 0;

  for (const savedDevice of pairedDevicesCache) {
    const device = mergedLiveDevice(savedDevice);
    if (device.online) onlineCount += 1;
    const readable = readableStateEntries(device).length > 0;
    if (readable) readableCount += 1;
    else rawOnlyCount += 1;
    let raw = {};
    try {
      raw = await apiFetch(`/api/devices/${encodeURIComponent(device.id)}/raw`, { cache: "no-store" });
    } catch {
      raw = { raw: device.raw || {} };
    }
    if (device.raw) {
      raw = { ...raw, raw: { ...(raw.raw || {}), ...device.raw } };
    }
    deviceRawCache[device.id] = raw;
    list.appendChild(renderDeviceDataCard(device, raw));
  }

  $("data-count-online").textContent = onlineCount;
  $("data-count-readable").textContent = readableCount;
  $("data-count-raw").textContent = rawOnlyCount;
  $("data-count-packets").textContent = bleStatsCache.raw_packet_count ?? bleRawPacketsCache.length;
  $("data-last-packet").textContent = formatLastSeen(bleStatsCache.last_packet_seen);
  $("ble-raw-status").textContent = bleStatsCache.scanning ? t("scanning") : t("idle");
  if (!list.childElementCount) list.textContent = t("noDeviceData");
  renderBleRawPacketTable();
  renderBleDiagnostics();
}

async function loadDeviceDataWithFeedback() {
  const button = $("device-data-refresh-button");
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("refreshing");
  try {
    await loadDevices();
    await loadDeviceData();
    await loadBleDiagnostics();
    showToast(t("refreshed"), "success");
  } catch (error) {
    showToast(`${t("refreshFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

async function collectBleDataWithFeedback() {
  const button = $("device-data-scan-button");
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("scanning");
  showToast(t("bleDataCollecting"), "success");
  try {
    await postJson("/api/ble-sensors/scan/start");
    await new Promise((resolve) => setTimeout(resolve, 48000));
    await loadDevices();
    await loadDeviceData();
    await loadBleDiagnostics();
    showToast(t("bleDataCollected"), "success");
  } catch (error) {
    showToast(`${t("bleDataCollectFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

function renderSensorCard(sensor, mode) {
  const card = document.createElement("article");
  card.className = "sensor-card";
  const title = document.createElement("div");
  title.className = "sensor-title";
  title.innerHTML = `<strong>${escapeText(sensor.display_name || sensor.name || sensor.mac)}</strong><code>${escapeText(sensor.mac)}</code>`;
  card.appendChild(title);

  const meta = document.createElement("p");
  meta.className = "sensor-meta";
  meta.textContent = `${sensor.brand || "unknown"} / ${sensor.model || "unknown"} / ${sensor.protocol || sensor.sensor_type || "unknown"} | ${t("signalStrength")} ${sensor.rssi ?? "-"} dBm | ${t("lastSeen")} ${sensor.last_seen || 0}s | ${sensor.parse_status || "unknown"}`;
  card.appendChild(meta);

  const value = document.createElement("p");
  value.className = "sensor-reading";
  value.textContent = sensorValue(sensor);
  card.appendChild(value);

  if (sensor.raw_advertisement) {
    const raw = document.createElement("details");
    raw.innerHTML = `<summary>${t("rawPacket")}</summary><code>${escapeText(sensor.raw_advertisement)}</code>`;
    card.appendChild(raw);
  }

  const actions = document.createElement("div");
  actions.className = "row sensor-actions";
  if (mode === "discovered" && !sensor.paired) {
    const pair = document.createElement("button");
    pair.type = "button";
    pair.className = "secondary small";
    pair.textContent = t("addSensor");
    pair.dataset.action = "pair";
    pair.dataset.mac = sensor.mac;
    pair.dataset.name = sensor.name || sensor.mac;
    pair.dataset.brand = sensor.brand || "unknown";
    pair.dataset.model = sensor.model || "unknown";
    pair.dataset.protocol = sensor.protocol || sensor.sensor_type || "unknown";
    actions.appendChild(pair);
  }
  if (mode === "paired") {
    const input = document.createElement("input");
    input.value = sensor.display_name || sensor.name || sensor.mac;
    input.maxLength = 32;
    input.dataset.renameInput = sensor.mac;
    actions.appendChild(input);

    const rename = document.createElement("button");
    rename.type = "button";
    rename.className = "secondary small";
    rename.textContent = t("rename");
    rename.dataset.action = "rename";
    rename.dataset.mac = sensor.mac;
    actions.appendChild(rename);

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "danger small";
    remove.textContent = t("remove");
    remove.dataset.action = "remove";
    remove.dataset.mac = sensor.mac;
    actions.appendChild(remove);

    const location = document.createElement("select");
    location.dataset.locationInput = sensor.mac;
    for (const value of LOCATIONS) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = value;
      option.selected = value === sensor.location;
      location.appendChild(option);
    }
    const customOption = document.createElement("option");
    customOption.value = sensor.location && !LOCATIONS.includes(sensor.location) ? sensor.location : "Custom";
    customOption.textContent = sensor.location && !LOCATIONS.includes(sensor.location) ? sensor.location : "Custom";
    customOption.selected = sensor.location && !LOCATIONS.includes(sensor.location);
    location.appendChild(customOption);
    actions.appendChild(location);

    const saveLocation = document.createElement("button");
    saveLocation.type = "button";
    saveLocation.className = "secondary small";
    saveLocation.textContent = t("location");
    saveLocation.dataset.action = "location";
    saveLocation.dataset.mac = sensor.mac;
    actions.appendChild(saveLocation);

    if (sensor.parse_status === "encrypted") {
      const bindkey = document.createElement("input");
      bindkey.placeholder = "32-char bindkey";
      bindkey.maxLength = 32;
      bindkey.dataset.bindkeyInput = sensor.mac;
      actions.appendChild(bindkey);

      const saveBindkey = document.createElement("button");
      saveBindkey.type = "button";
      saveBindkey.className = "secondary small";
      saveBindkey.textContent = t("bindkey");
      saveBindkey.dataset.action = "bindkey";
      saveBindkey.dataset.mac = sensor.mac;
      actions.appendChild(saveBindkey);
    }
  }
  if (actions.childElementCount) card.appendChild(actions);
  return card;
}

function isUnknownDevice(device) {
  const brand = String(device.brand || "").toLowerCase();
  const model = String(device.model || "").toLowerCase();
  const parseStatus = String(device.raw?.parse_status || device.parse_status || "").toLowerCase();
  const protocol = String(device.raw?.parser_protocol || device.protocol || "").toLowerCase();
  return brand === "unknown" || model === "unknown" || protocol === "unknown" || parseStatus === "unknown";
}

function signalClass(rssi) {
  const value = Number(rssi ?? -100);
  if (value >= -55) return "strong";
  if (value >= -72) return "good";
  return "weak";
}

function rawRssi(device) {
  return Number(device.raw?.rssi ?? device.rssi ?? -100);
}

function deviceLastSeen(device) {
  return Number(device.last_seen || 0);
}

function deviceDisplayName(device) {
  return device.name || device.raw?.ble_name || device.address || device.id;
}

function macFromBleId(id) {
  const raw = String(id || "");
  if (!raw.startsWith("ble-")) return "";
  const hex = raw.slice(4);
  if (!/^[0-9a-fA-F]{12}$/.test(hex)) return "";
  return hex.match(/.{1,2}/g).join(":").toUpperCase();
}

function normalizedMac(value) {
  return String(value || "").replace(/[^0-9a-fA-F]/g, "").toUpperCase();
}

function deviceAddressKey(device) {
  return normalizedMac(device?.address || macFromBleId(device?.id) || device?.id);
}

function isWatchedDevice(device) {
  const key = deviceAddressKey(device);
  return pairedDevicesCache.some((watched) => watched.id === device.id || (key && deviceAddressKey(watched) === key));
}

function liveDiscoveredForDevice(device) {
  const wanted = normalizedMac(device.address || device.id);
  if (!wanted) return null;
  return discoveredDevicesCache.find((item) => {
    const address = item.address || macFromBleId(item.id) || item.id;
    return normalizedMac(address) === wanted;
  }) || null;
}

function liveBlePacketForDevice(device) {
  const wanted = normalizedMac(device.address || device.id);
  if (!wanted) return null;
  return bleRawPacketsCache.find((packet) => {
    const address = packet.mac || packet.sensor_mac || packet.address;
    return normalizedMac(address) === wanted;
  }) || null;
}

function mergedLiveDevice(device) {
  const live = liveDiscoveredForDevice(device);
  const packet = liveBlePacketForDevice(device);
  if (!live && !packet) return device;
  const packetState = packet ? stateFromBlePacket(packet) : {};
  const packetRaw = packet ? rawFromBlePacket(packet) : {};
  const merged = {
    ...device,
    brand: device.brand && device.brand !== "generic" && device.brand !== "unknown" ? device.brand : (live?.brand || packet?.brand || device.brand),
    model: device.model && device.model !== "manual" && device.model !== "unknown" ? device.model : (live?.model || packet?.model || device.model),
    protocol: packetRaw.parser_protocol || live?.raw?.parser_protocol || live?.protocol || device.protocol,
    online: Boolean(packet || live?.online || device.online),
    last_seen: packet?.last_seen || live?.last_seen || device.last_seen,
    capabilities: live?.capabilities?.length ? live.capabilities : [...(device.capabilities || [])],
    state: Object.keys(packetState).length ? packetState : ((live?.state && Object.keys(live.state).length) ? live.state : device.state),
    raw: { ...(device.raw || {}), ...(live?.raw || {}), ...packetRaw },
  };
  if (packet?.temperature_c !== undefined && !merged.capabilities.includes("temperature")) merged.capabilities.push("temperature");
  if (packet?.external_probe_temperature_c !== undefined && !merged.capabilities.includes("external_probe_temperature")) merged.capabilities.push("external_probe_temperature");
  if (packet?.humidity_percent !== undefined && !merged.capabilities.includes("humidity")) merged.capabilities.push("humidity");
  if (packet?.battery_percent !== undefined && !merged.capabilities.includes("battery")) merged.capabilities.push("battery");
  if (packet && !merged.capabilities.length) merged.capabilities.push("raw");
  return merged;
}

function renderPairedDeviceCard(device) {
  const card = document.createElement("article");
  card.className = "device-card";
  const health = deviceHealth(device);
  const title = document.createElement("div");
  title.className = "device-card-title";
  title.innerHTML = `<div><strong>${escapeText(deviceDisplayName(device))}</strong><code>${escapeText(device.address || device.id)}</code></div><span class="status-pill ${health.cls}">${escapeText(health.label)}</span>`;
  card.appendChild(title);

  const meta = document.createElement("p");
  meta.className = "sensor-meta";
  meta.textContent = `${device.brand || "generic"} ${device.model || ""} | ${t("protocol")} ${device.protocol || "unknown"} | ${t("rssi")}: ${device.raw?.rssi ?? "-"} dBm | ${health.detail}`;
  card.appendChild(meta);

  const reading = document.createElement("p");
  reading.className = "sensor-reading";
  reading.textContent = deviceStateSummary(device);
  card.appendChild(reading);

  const state = document.createElement("details");
  state.innerHTML = `<summary>${t("stateAndRaw")}</summary><code>${escapeText(JSON.stringify({
    state: device.state || {},
    capabilities: device.capabilities || [],
    raw: device.raw || {},
  }, null, 2))}</code>`;
  card.appendChild(state);

  const actions = document.createElement("div");
  actions.className = "row sensor-actions";
  const name = document.createElement("input");
  name.value = device.name || device.id;
  name.maxLength = 32;
  name.dataset.renameDeviceInput = device.id;
  actions.appendChild(name);

  const rename = document.createElement("button");
  rename.type = "button";
  rename.className = "secondary small";
  rename.textContent = t("rename");
  rename.dataset.action = "rename-device";
  rename.dataset.id = device.id;
  actions.appendChild(rename);

  const location = document.createElement("input");
  location.placeholder = t("location");
  location.value = device.location || "";
  location.maxLength = 32;
  location.dataset.locationDeviceInput = device.id;
  actions.appendChild(location);

  const saveLocation = document.createElement("button");
  saveLocation.type = "button";
  saveLocation.className = "secondary small";
  saveLocation.textContent = t("location");
  saveLocation.dataset.action = "location-device";
  saveLocation.dataset.id = device.id;
  actions.appendChild(saveLocation);

  const listen = document.createElement("button");
  listen.type = "button";
  listen.className = "secondary small";
  listen.textContent = t("listenDevice");
  listen.dataset.action = "listen-device";
  listen.dataset.id = device.id;
  actions.appendChild(listen);

  const raw = document.createElement("button");
  raw.type = "button";
  raw.className = "secondary small";
  raw.textContent = t("rawData");
  raw.dataset.action = "raw-watched-device";
  raw.dataset.id = device.id;
  actions.appendChild(raw);

  if ((device.protocol || "").includes("ble") || (device.brand || "").toLowerCase().includes("inkbird")) {
    const gatt = document.createElement("button");
    gatt.type = "button";
    gatt.className = "secondary small";
    gatt.textContent = t("inspectGatt");
    gatt.dataset.action = "inspect-gatt-device";
    gatt.dataset.id = device.id;
    gatt.dataset.address = device.address || "";
    actions.appendChild(gatt);
  }

  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "danger small";
  remove.textContent = t("remove");
  remove.dataset.action = "remove-device";
  remove.dataset.id = device.id;
  actions.appendChild(remove);
  if (actions.childElementCount) card.appendChild(actions);
  return card;
}

function discoveredLatestValue(device) {
  const summary = deviceStateSummary(device);
  return summary === t("noState") ? "-" : summary;
}

function loadGattSnapshots() {
  try {
    gattSnapshotsCache = JSON.parse(localStorage.getItem(GATT_SNAPSHOTS_KEY) || "[]");
  } catch {
    gattSnapshotsCache = [];
  }
}

function saveGattSnapshots() {
  localStorage.setItem(GATT_SNAPSHOTS_KEY, JSON.stringify(gattSnapshotsCache.slice(0, 8)));
}

function flattenGattCharacteristics(inspection = {}) {
  const rows = [];
  for (const service of inspection.services || []) {
    for (const characteristic of service.characteristics || []) {
      rows.push({
        key: `${service.uuid}|${characteristic.uuid}|${characteristic.value_handle}`,
        service_uuid: service.uuid,
        service_handle: `${service.start_handle}-${service.end_handle}`,
        uuid: characteristic.uuid,
        handle: characteristic.value_handle,
        properties: characteristic.properties_text || "-",
        read_attempted: Boolean(characteristic.read_attempted),
        read_ok: Boolean(characteristic.read_ok),
        read_error: characteristic.read_error || 0,
        value_hex: characteristic.value_hex || "",
      });
    }
  }
  return rows;
}

function gattInspectionText(inspection = bleGattInspectionCache) {
  if (!inspection) return t("noGattInspection");
  const lines = [];
  lines.push(`mac=${inspection.mac || "-"}`);
  lines.push(`address_type=${inspection.address_type || "-"}`);
  lines.push(`ok=${Boolean(inspection.ok)} running=${Boolean(inspection.running)} error=${inspection.error || inspection.error_code || "-"}`);
  lines.push(`started_at=${inspection.started_at || "-"} completed_at=${inspection.completed_at || "-"}`);
  for (const row of flattenGattCharacteristics(inspection)) {
    lines.push(`${row.service_uuid} ${row.uuid} handle=${row.handle} props=${row.properties} read=${row.read_attempted ? (row.read_ok ? "ok" : `err:${row.read_error}`) : "-"} value=${row.value_hex || "-"}`);
  }
  return lines.join("\n");
}

function renderGattSnapshots() {
  const list = $("gatt-snapshots");
  if (!list) return;
  list.innerHTML = "";
  for (const snapshot of gattSnapshotsCache) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "secondary small gatt-snapshot-button";
    button.dataset.snapshotId = snapshot.id;
    button.textContent = `${snapshot.mac || "-"} · ${snapshot.saved_at_label || snapshot.saved_at || "-"}`;
    list.appendChild(button);
  }
  if (!list.childElementCount) {
    const empty = document.createElement("p");
    empty.className = "status-text";
    empty.textContent = t("noGattSnapshots");
    list.appendChild(empty);
  }
}

function compareGattSnapshot(snapshot) {
  const output = $("gatt-diff-output");
  if (!output) return;
  if (!snapshot || !bleGattInspectionCache) {
    output.textContent = "-";
    return;
  }
  const before = new Map(flattenGattCharacteristics(snapshot.inspection).map((row) => [row.key, row]));
  const after = flattenGattCharacteristics(bleGattInspectionCache);
  const lines = [];
  for (const current of after) {
    const previous = before.get(current.key);
    if (!previous) {
      lines.push(`+ ${current.service_uuid} ${current.uuid} handle=${current.handle} value=${current.value_hex || "-"}`);
      continue;
    }
    if ((previous.value_hex || "") !== (current.value_hex || "")) {
      lines.push(`* ${current.service_uuid} ${current.uuid} handle=${current.handle}`);
      lines.push(`  before=${previous.value_hex || "-"}`);
      lines.push(`  after =${current.value_hex || "-"}`);
    }
  }
  output.textContent = lines.join("\n") || t("noGattChanges");
}

function renderGattInspection() {
  const table = $("gatt-characteristics-table");
  const summary = $("gatt-summary");
  if (!table || !summary) return;

  const inspection = bleGattInspectionCache;
  const rows = flattenGattCharacteristics(inspection || {});
  $("gatt-characteristic-count").textContent = String(rows.length);
  summary.textContent = inspection
    ? `${inspection.mac || "-"} · ${inspection.ok ? t("ok") : inspection.running ? t("running") : t("failed")} · ${rows.length} ${t("characteristics")} · ${t("completedAt")}: ${inspection.completed_at || "-"}s`
    : t("noGattInspection");

  table.innerHTML = "";
  for (const row of rows) {
    const tr = document.createElement("tr");
    const readLabel = !row.read_attempted
      ? t("notReadable")
      : row.read_ok
        ? (row.value_hex || "-")
        : `${t("readFailed")}: ${row.read_error}`;
    tr.innerHTML = `
      <td><code>${escapeText(row.service_uuid)}</code><small>${escapeText(row.service_handle)}</small></td>
      <td><code>${escapeText(row.uuid)}</code><small>handle ${escapeText(row.handle)}</small></td>
      <td>${escapeText(row.properties)}</td>
      <td><code class="gatt-value">${escapeText(readLabel)}</code></td>
    `;
    table.appendChild(tr);
  }
  if (!table.childElementCount) {
    table.innerHTML = `<tr><td colspan="4">${escapeText(t("noGattInspection"))}</td></tr>`;
  }
  renderGattSnapshots();
  compareGattSnapshot(gattSnapshotsCache[0]);
}

function addPairDataset(button, device) {
  button.dataset.action = "pair-device";
  button.dataset.id = device.id;
  button.dataset.address = device.address || macFromBleId(device.id);
  button.dataset.name = deviceDisplayName(device);
  button.dataset.brand = device.brand || "generic";
  button.dataset.model = device.model || "unknown";
  button.dataset.protocol = device.protocol || "unknown";
  button.dataset.parserProtocol = device.raw?.parser_protocol || device.protocol || "unknown";
}

async function pairBleDevice(payload) {
  const address = payload.address || macFromBleId(payload.id);
  const universalPayload = { ...payload, address };
  if (!address) {
    throw new Error("BLE MAC address is missing. Open Raw data and check whether the scan result has an address.");
  }
  try {
    await postJson("/api/devices/pair", universalPayload);
    return;
  } catch (error) {
    if (!address) throw error;
    try {
      await postJson("/api/ble-sensors/pair", {
        mac: address,
        name: payload.name || address,
        brand: payload.brand || "unknown",
        model: payload.model || "unknown",
        protocol: payload.parser_protocol || payload.protocol || "unknown",
        location: "",
      });
    } catch (fallbackError) {
      await postJson("/api/devices/pair", {
        protocol: "ble_raw",
        address,
        name: payload.name || address,
        brand: payload.brand || "unknown",
        model: payload.model || "unknown",
        type: "sensor",
        location: "",
      });
      showToast(`${t("savedAsRawBle")}: ${fallbackError.message}`, "success");
    }
  }
}

async function pairBleDeviceFromButton(button) {
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("adding");
  try {
    await pairBleDevice({
      id: button.dataset.id,
      protocol: "ble",
      address: button.dataset.address,
      name: button.dataset.name,
      brand: button.dataset.brand,
      model: button.dataset.model,
      parser_protocol: button.dataset.parserProtocol,
    });
    showToast(t("devicePaired"), "success");
    await loadDevices();
    await loadBleSensors();
  } catch (error) {
    showToast(`${t("addDeviceFailed")}: ${error.message}`, "error");
    $("device-page-summary").textContent = `${t("addDeviceFailed")}: ${error.message}`;
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

function renderDiscoveredRow(device) {
  const row = document.createElement("article");
  row.className = "discovered-row";
  row.dataset.id = device.id;
  const rssi = rawRssi(device);
  const parseStatus = device.raw?.parse_status || device.parse_status || "-";
  const bleName = device.raw?.ble_name || device.name || "-";
  row.innerHTML = `
    <button class="signal ${signalClass(rssi)}" type="button" data-action="details" title="${t("rawData")}"><span></span><span></span><span></span></button>
    <div><strong>${escapeText(device.brand || "unknown")} ${escapeText(device.model || "")}</strong><small>${escapeText(device.type || "unknown")}</small></div>
    <div>${escapeText(bleName)}</div>
    <code>${escapeText(device.address || device.id)}</code>
    <div>${escapeText(device.raw?.parser_protocol || device.protocol || "unknown")}</div>
    <div>${rssi} dBm</div>
    <span class="status-pill ${isUnknownDevice(device) ? "is-offline" : "is-online"}">${escapeText(parseStatus)}</span>
    <div class="latest-value">${escapeText(discoveredLatestValue(device))}</div>
    <div class="row row-actions">
      <button class="secondary small" type="button" data-action="raw-device">${t("rawData")}</button>
      <button class="small" type="button" data-action="pair-device">${t("addDevice")}</button>
    </div>
  `;
  addPairDataset(row.querySelector("[data-action='pair-device']"), device);
  return row;
}

function filteredDiscoveredDevices() {
  const query = $("device-search").value.trim().toLowerCase();
  const filter = $("device-filter").value;
  const sort = $("device-sort").value;
  const hideUnknown = $("hide-unknown-devices").checked;
  const onlyNew = $("only-new-devices").checked;
  let devices = discoveredDevicesCache.filter((device) => {
    const haystack = `${device.id} ${device.address || ""} ${device.name || ""} ${device.raw?.ble_name || ""} ${device.brand || ""} ${device.model || ""}`.toLowerCase();
    const unknown = isUnknownDevice(device);
    if (query && !haystack.includes(query)) return false;
    if (hideUnknown && unknown) return false;
    if (isWatchedDevice(device)) return false;
    if (onlyNew && isWatchedDevice(device)) return false;
    if (filter === "known" && unknown) return false;
    if (filter === "unknown" && !unknown) return false;
    if (filter === "strong" && rawRssi(device) < -65) return false;
    if (filter === "recent" && deviceLastSeen(device) > 30) return false;
    return true;
  });
  devices.sort((a, b) => {
    if (sort === "last_seen") return deviceLastSeen(a) - deviceLastSeen(b);
    if (sort === "brand") return `${a.brand || ""} ${a.model || ""}`.localeCompare(`${b.brand || ""} ${b.model || ""}`);
    return rawRssi(b) - rawRssi(a);
  });
  return devices;
}

function updateDeviceCounts() {
  const visibleDiscovered = discoveredDevicesCache.filter((device) => !isWatchedDevice(device));
  const known = visibleDiscovered.filter((device) => !isUnknownDevice(device)).length;
  $("device-count-paired").textContent = pairedDevicesCache.length;
  $("device-count-discovered").textContent = visibleDiscovered.length;
  $("device-count-known").textContent = known;
  $("device-count-unknown").textContent = Math.max(0, visibleDiscovered.length - known);
}

function renderDevices() {
  updateDeviceCounts();
  const list = $("devices-list");
  list.innerHTML = "";
  for (const device of pairedDevicesCache) {
    list.appendChild(renderPairedDeviceCard(mergedLiveDevice(device)));
  }
  if (!list.childElementCount) list.textContent = t("noPairedDevices");

  const discoveredList = $("devices-discovered");
  discoveredList.innerHTML = "";
  const filtered = filteredDiscoveredDevices();
  const page = filtered.slice(0, discoveredVisibleLimit);
  for (const device of page) {
    discoveredList.appendChild(renderDiscoveredRow(device));
  }
  if (!discoveredList.childElementCount) discoveredList.textContent = t("noDiscoveredDevices");
  $("devices-show-more-button").hidden = filtered.length <= discoveredVisibleLimit;
  $("device-page-summary").textContent = filtered.length ? `${Math.min(discoveredVisibleLimit, filtered.length)} / ${filtered.length}` : "";
}

function openDeviceDrawer(device) {
  drawerDevice = device;
  const matchingPackets = bleRawPacketsCache.filter((packet) => normalizedMac(packet.mac || packet.sensor_mac || packet.address) === deviceAddressKey(device));
  const matchingGatt = bleGattInspectionCache &&
    normalizedMac(bleGattInspectionCache.mac) === deviceAddressKey(device)
    ? bleGattInspectionCache
    : null;
  $("drawer-device-name").textContent = deviceDisplayName(device);
  const rssi = rawRssi(device);
  const health = deviceHealth(device);
  $("drawer-device-meta").innerHTML = `
    <span>${escapeText(device.brand || "unknown")} ${escapeText(device.model || "")}</span>
    <span>${escapeText(device.protocol || "unknown")}</span>
    <span>${escapeText(device.address || device.id)}</span>
    <span>${t("rssi")}: ${rssi} dBm</span>
    <span>${escapeText(health.label)}</span>
    <span>${t("lastSeen")}: ${formatSeenAge(device.last_seen)}</span>
  `;
  $("drawer-decoded").value = JSON.stringify({
    id: device.id,
    name: device.name,
    ble_name: device.raw?.ble_name,
    parser_protocol: device.raw?.parser_protocol || device.protocol,
    parse_status: device.raw?.parse_status,
    health: health.label,
    diagnostic: health.detail,
    state: device.state || {},
    capabilities: device.capabilities || [],
    matching_raw_packet_count: matchingPackets.length,
  }, null, 2);
  $("drawer-raw").value = JSON.stringify({
    latest_raw: device.raw || {},
    recent_matching_packets: matchingPackets.slice(0, 8),
    gatt_inspection: matchingGatt,
  }, null, 2);
  $("drawer-add-device").hidden = isWatchedDevice(device);
  $("device-drawer").hidden = false;
}

async function waitForGattInspection(mac) {
  let latest = null;
  for (let attempt = 0; attempt < 24; attempt += 1) {
    latest = await apiFetch("/api/ble/gatt/inspection", { cache: "no-store" });
    bleGattInspectionCache = latest;
    if (!latest.running && normalizedMac(latest.mac) === normalizedMac(mac)) {
      return latest;
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  return latest;
}

async function inspectGattDevice(button) {
  const oldText = button.textContent;
  const id = button.dataset.id;
  const device = pairedDevicesCache.find((item) => item.id === id);
  const mac = device?.address || button.dataset.address || "";
  if (!mac || !mac.includes(":")) {
    showToast(t("gattInspectNoMac"), "error");
    return;
  }
  button.disabled = true;
  button.textContent = t("inspectingGatt");
  try {
    await postJson("/api/ble/gatt/inspect", { mac });
    showToast(t("gattInspectStarted"), "success");
    const inspection = await waitForGattInspection(mac);
    bleGattInspectionCache = inspection;
    const serviceCount = inspection?.services?.length || 0;
    showToast(
      inspection?.ok
        ? `${t("gattInspectDone")}: ${serviceCount} ${t("gattServices")}`
        : `${t("gattInspectFailed")}: ${inspection?.error || inspection?.error_code || "-"}`,
      inspection?.ok ? "success" : "error"
    );
    if (device) {
      openDeviceDrawer(mergedLiveDevice(device));
    }
    renderGattInspection();
  } catch (error) {
    showToast(`${t("gattInspectFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

function closeDeviceDrawer() {
  $("device-drawer").hidden = true;
  drawerDevice = null;
}

async function loadDevices() {
  const [devices, discovered] = await Promise.all([
    apiFetch("/api/devices", { cache: "no-store" }),
    apiFetch("/api/devices/discovered", { cache: "no-store" }),
  ]);
  pairedDevicesCache = devices.devices || [];
  discoveredDevicesCache = (discovered.devices || []).filter((device) => device.online || (device.capabilities || []).includes("raw"));
  renderDevices();
  renderGattInspection();
}

async function refreshDevicesWithFeedback() {
  const button = $("devices-refresh-button");
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("refreshing");
  $("device-page-summary").textContent = t("refreshing");
  try {
    await loadDevices();
    await loadDeviceData();
    await loadBleSensors();
    $("device-page-summary").textContent = t("refreshed");
    showToast(t("refreshed"), "success");
  } catch (error) {
    $("device-page-summary").textContent = `${t("refreshFailed")}: ${error.message}`;
    showToast(`${t("refreshFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

async function scanBleWithFeedback() {
  const button = $("devices-scan-ble-button");
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("scanning");
  $("device-page-summary").textContent = t("bleScanStarting");
  try {
    await postJson("/api/ble-sensors/scan/start");
    $("device-page-summary").textContent = t("bleScanStarted");
    showToast(t("bleScanStarted"), "success");
    await new Promise((resolve) => setTimeout(resolve, 48000));
    await loadDevices();
    await loadDeviceData();
    await loadBleSensors();
  } catch (error) {
    const message = `${t("bleScanFailed")}: ${error.message}`;
    $("device-page-summary").textContent = message;
    showToast(message, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

async function listenToDevice(button) {
  const oldText = button.textContent;
  const id = button.dataset.id;
  button.disabled = true;
  button.textContent = t("listening");
  try {
    await postJson("/api/ble-sensors/scan/start");
    showToast(t("listenStarted"), "success");
    await new Promise((resolve) => setTimeout(resolve, 48000));
    await loadDeviceData();
    await loadDevices();
    const device = pairedDevicesCache.find((item) => item.id === id);
    if (device) openDeviceDrawer(mergedLiveDevice(device));
  } catch (error) {
    showToast(`${t("listenFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
}

async function loadBleSensors() {
  const scan = await apiFetch("/api/ble-sensors/scan/status", { cache: "no-store" });
  $("ble-sensor-scan-state").textContent = scan.scanning ? t("scanning") : t("idle");
  $("ble-sensor-discovered-count").textContent = scan.discovered_count || 0;
  $("ble-sensor-paired-count").textContent = scan.paired_count || 0;

  const [discovered, paired] = await Promise.all([
    apiFetch("/api/ble-sensors/discovered", { cache: "no-store" }),
    apiFetch("/api/ble-sensors/latest", { cache: "no-store" }),
  ]);

  const discoveredList = $("ble-sensor-discovered");
  discoveredList.innerHTML = "";
  for (const sensor of discovered.sensors || []) {
    discoveredList.appendChild(renderSensorCard(sensor, "discovered"));
  }
  if (!discoveredList.childElementCount) discoveredList.textContent = t("noSensors");

  const pairedList = $("ble-sensor-paired");
  pairedList.innerHTML = "";
  for (const sensor of paired.sensors || []) {
    pairedList.appendChild(renderSensorCard(sensor, "paired"));
  }
  if (!pairedList.childElementCount) pairedList.textContent = t("noPairedSensors");
}

async function loadCloudOta() {
  const data = await apiFetch("/api/ota/config", { cache: "no-store" });
  $("cloud-ota-enabled").checked = Boolean(data.ota_enabled);
  $("cloud-ota-auto-update").checked = Boolean(data.auto_update);
  $("cloud-ota-url").value = data.ota_check_url || "";
  $("cloud-ota-channel").value = data.ota_channel || "stable";
  $("cloud-device-token").value = "";
  $("cloud-device-token").placeholder = data.device_token_set ? "Device token saved; leave blank to keep it" : "Paste device token from Tiger Cloud";
  $("cloud-ota-latest").textContent = data.update_available ? (data.latest_version || "Available") : "Up to date";
  $("cloud-ota-progress-text").textContent = `${Number(data.progress || 0)}%`;
  $("cloud-ota-force").textContent = data.force ? "Yes" : "No";
  $("cloud-ota-notes").value = data.release_notes || data.last_error || "";
  setStatus("cloud-ota-status", data.last_error || "");
}

async function loadLogs() {
  const data = await apiFetch("/api/logs", { cache: "no-store" });
  logsCache = data.logs || [];
  $("logs").textContent = (data.logs || [])
    .map((item) => `[${item.uptime_seconds}s] ${item.level} ${item.tag}: ${item.message}`)
    .join("\n") || "No logs yet.";
  renderWebhookLogs();
  if ($("device-data-diagnostics")) {
    renderBleDiagnostics();
  }
  if ($("diagnostics-output")) {
    renderSystemDiagnostics();
  }
}

function renderWebhookLogs() {
  const output = $("webhook-logs");
  if (!output) return;
  const items = logsCache
    .filter((item) => {
      const text = `${item.tag || ""} ${item.message || ""}`.toLowerCase();
      return text.includes("webhook") || text.includes("https webhook");
    })
    .slice(-12);
  output.textContent = items
    .map((item) => `[${item.uptime_seconds}s] ${item.level} ${item.tag}: ${item.message}`)
    .join("\n") || t("noWebhookLogs");

  const summary = $("webhook-log-summary");
  if (!summary) return;
  const last = [...items].reverse().find((item) =>
    /Webhook (posted|POST failed|background send complete|skipped)/.test(item.message || "")
  );
  summary.textContent = last
    ? `[${last.uptime_seconds}s] ${last.level}: ${last.message}`
    : t("noWebhookLogs");
}

async function scanNetworksInto(selectId = "wifi-networks") {
  const select = $(selectId);
  select.innerHTML = `<option value="">${t("scanning")}...</option>`;

  const response = await fetch("/api/wifi/scan", { cache: "no-store" });
  const data = await response.json();
  const networks = data.networks || [];

  select.innerHTML = `<option value="">${t("chooseNetwork")}</option>`;
  for (const network of networks) {
    const option = document.createElement("option");
    option.value = network.ssid;
    option.textContent = `${network.ssid} (${network.rssi} dBm, ${network.auth_mode})`;
    select.appendChild(option);
  }
}

async function scanNetworks() {
  return scanNetworksInto("wifi-networks");
}

async function postJson(url, payload = {}) {
  return apiFetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
}

function mqttPayload() {
  return {
    enabled: $("mqtt-enabled").checked,
    host: $("mqtt-host").value.trim(),
    port: Number($("mqtt-port").value || 0),
    username: $("mqtt-username").value.trim(),
    password: $("mqtt-password").value,
    client_id: $("mqtt-client-id").value.trim(),
    use_tls: $("mqtt-use-tls").checked,
    ha_discovery_enabled: $("ha-discovery-enabled").checked,
    ha_discovery_prefix: $("ha-discovery-prefix").value.trim() || "homeassistant",
  };
}

function webhookPayload() {
  return {
    enabled: $("webhook-enabled").checked,
    url: $("webhook-url").value.trim(),
    secret_header: $("webhook-secret-header").value.trim() || "x-ingest-secret",
    secret_value: $("webhook-secret-value").value,
    interval_seconds: Number($("webhook-interval").value || 300),
  };
}

function blePayload() {
  return {
    enabled: $("ble-enabled").checked,
    pairing_pin: $("ble-pairing-pin").value.trim(),
    pop_token: $("ble-pop-token").value,
  };
}

function cloudOtaPayload() {
  return {
    ota_enabled: $("cloud-ota-enabled").checked,
    auto_update: $("cloud-ota-auto-update").checked,
    ota_check_url: $("cloud-ota-url").value.trim(),
    ota_channel: $("cloud-ota-channel").value,
    device_token: $("cloud-device-token").value,
  };
}

async function waitForDeviceAndReload() {
  for (let attempt = 0; attempt < 18; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 2000));
    try {
      const response = await fetch("/api/status", { cache: "no-store" });
      if (response.ok) {
        window.location.reload();
        return;
      }
    } catch {
      // Device is still rebooting.
    }
  }
  setStatus("ota-status", "Device is still rebooting. Refresh this page after it comes back online.", "error");
}

async function monitorCloudOtaInstall() {
  for (let attempt = 0; attempt < 240; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 1000));
    try {
      await loadCloudOta();
      const text = $("cloud-ota-progress-text").textContent || "0%";
      if (text.startsWith("100")) {
        setStatus("cloud-ota-status", "Cloud OTA installed. Waiting for reboot...", "success");
        await waitForDeviceAndReload();
        return;
      }
    } catch {
      await waitForDeviceAndReload();
      return;
    }
  }
  setStatus("cloud-ota-status", "Cloud OTA is still running. Check logs before retrying.", "error");
}

$("login-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    const data = await postJson("/api/login", {
      username: $("login-username").value.trim(),
      password: $("login-password").value,
    });
    apiToken = data.token;
    localStorage.setItem(TOKEN_KEY, apiToken);
    showDashboard(true);
    showToast("Login successful.");
    await loadStatus();
    await loadWebhook();
    await loadMqtt();
    await loadBle();
    await loadHardware();
    await loadDevices();
    await loadBleSensors();
    await loadCloudOta();
    await loadLogs();
    await scanNetworks();
  } catch (error) {
    setStatus("login-status", error.message, "error");
  }
});

$("onboarding-scan-button").addEventListener("click", async () => {
  try {
    await scanNetworksInto("onboarding-wifi-networks");
  } catch (error) {
    setStatus("onboarding-status", `${t("scanFailed")}: ${error.message}`, "error");
  }
});

$("onboarding-wifi-networks").addEventListener("change", (event) => {
  $("onboarding-ssid").value = event.target.value;
});

$("onboarding-wifi-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  onboardingActive = true;
  const button = $("onboarding-connect-button");
  button.disabled = true;
  setStatus("onboarding-status", t("connectingWifi"));
  try {
    await postJson("/api/wifi", {
      ssid: $("onboarding-ssid").value.trim(),
      password: $("onboarding-password").value,
    });
    setStatus("onboarding-status", t("wifiSavedWaitingIp"), "success");
    await loadStatus();
  } catch (error) {
    setStatus("onboarding-status", `${t("wifiConnectFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
  }
});

$("logout-button").addEventListener("click", () => {
  apiToken = "";
  localStorage.removeItem(TOKEN_KEY);
  showDashboard(false);
});

$("language-select").addEventListener("change", (event) => {
  currentLanguage = event.target.value;
  localStorage.setItem(LANG_KEY, currentLanguage);
  applyI18n();
  loadStatus().catch(() => {});
  if (apiToken) {
    renderDevices();
    loadDeviceData().catch(() => {});
    loadBleSensors().catch(() => {});
  }
});

document.querySelectorAll("[data-tab]").forEach((button) => {
  button.addEventListener("click", () => {
    switchTab(button.dataset.tab);
    if (button.dataset.tab === "data" && apiToken) {
      loadDeviceData().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
      loadBleDiagnostics().catch(() => {});
    }
    if (button.dataset.tab === "diagnostics" && apiToken) {
      loadSystemDiagnostics().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
    }
    if (button.dataset.tab === "hardware" && apiToken) {
      loadHardware().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
    }
  });
});

$("scan-button").addEventListener("click", scanNetworks);
$("refresh-logs-button").addEventListener("click", loadLogs);
$("hardware-refresh-button").addEventListener("click", () => {
  loadHardware().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
});

$("display-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  await postJson("/api/hardware/display", {
    enabled: $("display-enabled").checked,
    backlight: $("display-backlight").checked,
  });
  showToast(t("displaySettingsSaved"), "success");
  await loadHardware();
});

$("beep-test-button").addEventListener("click", async () => {
  await postJson("/api/hardware/beep", { kind: "test" });
  showToast(t("beepRequestLogged"), "success");
});

$("devices-refresh-button").addEventListener("click", refreshDevicesWithFeedback);
$("device-data-refresh-button").addEventListener("click", loadDeviceDataWithFeedback);
$("device-data-scan-button").addEventListener("click", collectBleDataWithFeedback);
$("device-data-stop-button").addEventListener("click", async () => {
  await postJson("/api/ble-sensors/scan/stop");
  await loadDeviceData();
  showToast(t("bleScanStopped"), "success");
});
$("device-data-clear-raw-button").addEventListener("click", async () => {
  await postJson("/api/ble/raw/clear");
  bleRawPacketsCache = [];
  await loadDeviceData();
  showToast(t("rawPacketsCleared"), "success");
});
$("device-data-logs-refresh-button").addEventListener("click", (event) => {
  event.preventDefault();
  loadBleDiagnostics().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
});
$("refresh-diagnostics-button").addEventListener("click", () => {
  loadSystemDiagnostics().catch((error) => showToast(`${t("refreshFailed")}: ${error.message}`, "error"));
});
$("diagnostics-filter").addEventListener("change", renderSystemDiagnostics);
$("copy-diagnostics-button").addEventListener("click", async () => {
  const output = $("diagnostics-output");
  const text = diagnosticsCache || output.textContent || "";
  try {
    await copyTextWithFallback(text, output);
    showToast(t("logsCopied"), "success");
  } catch (error) {
    showToast(error.message === "manual" ? t("copyManual") : t("copyFailed"), "error");
  }
});
$("clear-diagnostics-button").addEventListener("click", async () => {
  await postJson("/api/logs/clear");
  logsCache = [];
  await loadSystemDiagnostics();
  showToast(t("logsCleared"), "success");
});

["device-search", "device-filter", "device-sort", "hide-unknown-devices", "only-new-devices"].forEach((id) => {
  $(id).addEventListener("input", () => {
    discoveredVisibleLimit = 10;
    renderDevices();
  });
  $(id).addEventListener("change", () => {
    discoveredVisibleLimit = 10;
    renderDevices();
  });
});

$("devices-show-more-button").addEventListener("click", () => {
  discoveredVisibleLimit += 10;
  renderDevices();
});

$("devices-scan-ble-button").addEventListener("click", scanBleWithFeedback);

$("gatt-refresh-button").addEventListener("click", async () => {
  try {
    bleGattInspectionCache = await apiFetch("/api/ble/gatt/inspection", { cache: "no-store" });
    renderGattInspection();
    showToast(t("refreshed"), "success");
  } catch (error) {
    showToast(`${t("refreshFailed")}: ${error.message}`, "error");
  }
});

$("gatt-copy-button").addEventListener("click", async () => {
  const output = $("gatt-diff-output");
  try {
    await copyTextWithFallback(gattInspectionText(), output);
    showToast(t("logsCopied"), "success");
  } catch (error) {
    showToast(error.message === "manual" ? t("copyManual") : t("copyFailed"), "error");
  }
});

$("gatt-save-snapshot-button").addEventListener("click", () => {
  if (!bleGattInspectionCache || !bleGattInspectionCache.services?.length) {
    showToast(t("noGattInspection"), "error");
    return;
  }
  const now = new Date();
  gattSnapshotsCache.unshift({
    id: `${Date.now()}`,
    mac: bleGattInspectionCache.mac || "",
    saved_at: now.toISOString(),
    saved_at_label: now.toLocaleTimeString(),
    inspection: bleGattInspectionCache,
  });
  gattSnapshotsCache = gattSnapshotsCache.slice(0, 8);
  saveGattSnapshots();
  renderGattInspection();
  showToast(t("gattSnapshotSaved"), "success");
});

$("gatt-snapshots").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-snapshot-id]");
  if (!button) return;
  const snapshot = gattSnapshotsCache.find((item) => item.id === button.dataset.snapshotId);
  compareGattSnapshot(snapshot);
});

$("devices-discovered").addEventListener("click", async (event) => {
  const raw = event.target.closest("button[data-action='raw-device'], button[data-action='details']");
  if (raw) {
    const row = event.target.closest("[data-id]");
    const device = discoveredDevicesCache.find((item) => item.id === row?.dataset.id);
    if (device) openDeviceDrawer(device);
    return;
  }
  const rowClick = event.target.closest(".discovered-row");
  const button = event.target.closest("button[data-action='pair-device']");
  if (!button && rowClick) {
    const device = discoveredDevicesCache.find((item) => item.id === rowClick.dataset.id);
    if (device) openDeviceDrawer(device);
    return;
  }
  if (!button) return;
  await pairBleDeviceFromButton(button);
});

$("devices-list").addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button) return;
  if (button.dataset.action === "listen-device") {
    await listenToDevice(button);
    return;
  }
  if (button.dataset.action === "raw-watched-device") {
    await loadDeviceData();
    const device = pairedDevicesCache.find((item) => item.id === button.dataset.id);
    if (device) openDeviceDrawer(mergedLiveDevice(device));
    return;
  }
  if (button.dataset.action === "inspect-gatt-device") {
    await inspectGattDevice(button);
    return;
  }
  if (button.dataset.action === "remove-device") {
    if (!confirm(t("confirmRemoveDevice"))) return;
    await postJson("/api/devices/remove", { id: button.dataset.id });
    showToast(t("deviceRemoved"));
  }
  if (button.dataset.action === "rename-device") {
    const input = document.querySelector(`[data-rename-device-input="${button.dataset.id}"]`);
    await postJson("/api/devices/rename", { id: button.dataset.id, name: input.value.trim() });
    showToast(t("deviceRenamed"));
  }
  if (button.dataset.action === "location-device") {
    const input = document.querySelector(`[data-location-device-input="${button.dataset.id}"]`);
    await postJson("/api/devices/location", { id: button.dataset.id, location: input.value.trim() });
    showToast(t("deviceLocationSaved"));
  }
  await loadDevices();
  await loadBleSensors();
});

document.querySelectorAll("[data-drawer-close]").forEach((element) => {
  element.addEventListener("click", closeDeviceDrawer);
});

$("drawer-add-device").addEventListener("click", async () => {
  if (!drawerDevice) return;
  const button = $("drawer-add-device");
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = t("adding");
  try {
    await pairBleDevice({
      id: drawerDevice.id,
      protocol: "ble",
      address: drawerDevice.address || macFromBleId(drawerDevice.id),
      name: deviceDisplayName(drawerDevice),
      brand: drawerDevice.brand || "generic",
      model: drawerDevice.model || "unknown",
      parser_protocol: drawerDevice.raw?.parser_protocol || drawerDevice.protocol || "unknown",
    });
    showToast(t("devicePaired"), "success");
    closeDeviceDrawer();
    await loadDevices();
    await loadBleSensors();
  } catch (error) {
    showToast(`${t("addDeviceFailed")}: ${error.message}`, "error");
  } finally {
    button.disabled = false;
    button.textContent = oldText;
  }
});

$("add-http-device-button").addEventListener("click", async () => {
  const ip = $("http-device-ip").value.trim();
  if (!ip) return;
  await postJson("/api/devices/pair", {
    protocol: "http",
    address: ip,
    name: `HTTP ${ip}`,
    brand: "generic",
    model: "manual",
  });
  showToast("HTTP device placeholder saved.");
  await loadDevices();
});

$("ble-sensor-scan-button").addEventListener("click", async () => {
  await postJson("/api/ble-sensors/scan/start");
  showToast("BLE sensor scan started.");
  await loadBleSensors();
});

$("ble-sensor-stop-button").addEventListener("click", async () => {
  await postJson("/api/ble-sensors/scan/stop");
  showToast("BLE sensor scan stopped.");
  await loadBleSensors();
});

$("ble-sensor-discovered").addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-action='pair']");
  if (!button) return;
  await postJson("/api/ble-sensors/pair", {
    mac: button.dataset.mac,
    name: button.dataset.name,
    brand: button.dataset.brand,
    model: button.dataset.model,
    protocol: button.dataset.protocol,
    location: "",
  });
  showToast(t("devicePaired"));
  await loadBleSensors();
});

$("ble-sensor-paired").addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button) return;
  const mac = button.dataset.mac;
  if (button.dataset.action === "rename") {
    const input = document.querySelector(`[data-rename-input="${mac}"]`);
    await postJson("/api/ble-sensors/rename", { mac, name: input.value.trim() });
    showToast("BLE sensor renamed.");
  }
  if (button.dataset.action === "remove") {
    if (!confirm("Remove this BLE sensor from the watched list?")) return;
    await postJson("/api/ble-sensors/remove", { mac });
    showToast("BLE sensor removed.");
  }
  if (button.dataset.action === "location") {
    const input = document.querySelector(`[data-location-input="${mac}"]`);
    await postJson("/api/ble-sensors/location", { mac, location: input.value });
    showToast("BLE sensor location saved.");
  }
  if (button.dataset.action === "bindkey") {
    const input = document.querySelector(`[data-bindkey-input="${mac}"]`);
    await postJson("/api/ble-sensors/bindkey", { mac, bindkey: input.value.trim() });
    showToast("BLE sensor bindkey saved.");
  }
  await loadBleSensors();
});

$("mqtt-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  await postJson("/api/mqtt", mqttPayload());
  setStatus("mqtt-form-status", "MQTT settings saved. Reconnect will happen automatically when WiFi is online.", "success");
  showToast("MQTT settings saved.");
  await loadMqtt();
});

$("webhook-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  await postJson("/api/webhook", webhookPayload());
  webhookFormDirty = false;
  setStatus("webhook-form-status", t("webhookSettingsSaved"), "success");
  showToast(t("webhookSettingsSaved"), "success");
  await loadWebhook();
});

$("webhook-test-button").addEventListener("click", async () => {
  await postJson("/api/webhook", webhookPayload());
  webhookFormDirty = false;
  setStatus("webhook-form-status", t("webhookTesting"));
  const data = await postJson("/api/webhook/test");
  const detail = data.sent_count !== undefined
    ? `${t("webhookTestSent")} ${data.sent_count} sent, ${data.skipped_count || 0} skipped.`
    : t("webhookTestSent");
  setStatus("webhook-form-status", detail, "success");
  showToast(detail, "success");
  await loadWebhook();
  setTimeout(() => loadLogs().catch(() => {}), 3500);
  setTimeout(() => loadLogs().catch(() => {}), 9000);
});

$("webhook-logs-refresh-button").addEventListener("click", loadLogs);

$("webhook-form").addEventListener("input", () => {
  webhookFormDirty = true;
});

$("webhook-form").addEventListener("focusin", () => {
  webhookFormDirty = true;
});

$("mqtt-test-button").addEventListener("click", async () => {
  await postJson("/api/mqtt", mqttPayload());
  showToast("MQTT reconnect requested.");
  setStatus("mqtt-form-status", "Reconnect requested. Watch MQTT status and logs.", "success");
  setTimeout(() => loadMqtt().catch(() => {}), 2500);
});

$("ble-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const pin = $("ble-pairing-pin").value.trim();
  if (pin.length !== 6 || !/^[0-9]+$/.test(pin)) {
    setStatus("ble-form-status", "Pairing PIN must be exactly 6 digits.", "error");
    return;
  }
  await postJson("/api/ble", blePayload());
  setStatus("ble-form-status", "BLE settings saved.", "success");
  showToast("BLE settings saved.");
  await loadBle();
});

$("cloud-ota-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  await postJson("/api/ota/config", cloudOtaPayload());
  setStatus("cloud-ota-status", "Cloud OTA settings saved.", "success");
  showToast("Cloud OTA settings saved.");
  await loadCloudOta();
});

$("cloud-ota-check-button").addEventListener("click", async () => {
  await postJson("/api/ota/config", cloudOtaPayload());
  setStatus("cloud-ota-status", "Checking for cloud OTA update...");
  const data = await postJson("/api/ota/check");
  $("cloud-ota-latest").textContent = data.update_available ? data.version : "Up to date";
  $("cloud-ota-force").textContent = data.force ? "Yes" : "No";
  $("cloud-ota-notes").value = data.release_notes || data.error || "";
  setStatus("cloud-ota-status", data.update_available ? "Update available." : "TigerOS is up to date.", data.update_available ? "success" : "");
  showToast(data.update_available ? "Cloud OTA update available." : "TigerOS is up to date.");
});

$("cloud-ota-update-button").addEventListener("click", async () => {
  if (!confirm("Install the latest cloud OTA update now?")) return;
  setStatus("cloud-ota-status", "Installing cloud OTA update...");
  await postJson("/api/ota/update");
  setStatus("cloud-ota-status", "Cloud OTA install started. Monitoring progress...");
  showToast("Cloud OTA install started.");
  monitorCloudOtaInstall();
});

$("wifi-networks").addEventListener("change", (event) => {
  if (event.target.value) $("ssid").value = event.target.value;
});

$("wifi-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const ssid = $("ssid").value.trim();
  const password = $("password").value;
  await postJson("/api/wifi", { ssid, password });
  showToast("WiFi credentials saved.");
  await loadStatus();
});

$("reboot-button").addEventListener("click", async () => {
  if (!confirm("Reboot TigerOS now?")) return;
  await postJson("/api/reboot");
  showToast("Reboot requested.");
});

$("factory-reset-button").addEventListener("click", async () => {
  if (confirm("Clear WiFi, cloud, MQTT, OTA, and BLE provisioning settings?")) {
    await postJson("/api/factory-reset");
    showToast("Factory reset requested.");
  }
});

$("ota-file").addEventListener("change", (event) => {
  setOtaFile(event.target.files[0]);
});

$("ota-drop-zone").addEventListener("dragover", (event) => {
  event.preventDefault();
  $("ota-drop-zone").classList.add("dragging");
});

$("ota-drop-zone").addEventListener("dragleave", () => {
  $("ota-drop-zone").classList.remove("dragging");
});

$("ota-drop-zone").addEventListener("drop", (event) => {
  event.preventDefault();
  $("ota-drop-zone").classList.remove("dragging");
  setOtaFile(event.dataTransfer.files[0]);
});

$("ota-button").addEventListener("click", async () => {
  const file = $("ota-file").files[0];
  if (!file) {
    setStatus("ota-status", "Choose a .bin firmware file first.", "error");
    showToast("Please choose a firmware .bin file first.", "error");
    return;
  }

  const progress = $("ota-progress");
  const button = $("ota-button");
  progress.value = 0;
  button.disabled = true;
  setStatus("ota-status", "Preparing OTA: pausing BLE scan...");
  try {
    await postJson("/api/ble-sensors/scan/stop");
    await new Promise((resolve) => setTimeout(resolve, 600));
  } catch {
    // Older firmware may fail this call; backend also pauses BLE before writing.
  }
  setStatus("ota-status", `Uploading ${file.name}...`);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/ota");
  xhr.setRequestHeader("Content-Type", "application/octet-stream");
  xhr.setRequestHeader("Authorization", `Bearer ${apiToken}`);
  xhr.upload.onprogress = (event) => {
    if (event.lengthComputable) {
      const percent = Math.round((event.loaded / event.total) * 100);
      progress.value = percent;
      setStatus("ota-status", `Uploading ${file.name}: ${percent}%`);
    }
  };
  xhr.onload = () => {
    button.disabled = false;
    if (xhr.status >= 200 && xhr.status < 300) {
      progress.value = 100;
      let message = "OTA upload complete. Device is rebooting now.";
      try {
        const data = JSON.parse(xhr.responseText || "{}");
        if (data.message) message = data.message;
      } catch {
        // Keep the default success message.
      }
      setStatus("ota-status", `${message} Waiting for TigerOS to come back online...`, "success");
      showToast("OTA uploaded successfully. TigerOS is rebooting.", "success");
      waitForDeviceAndReload();
      return;
    }

    progress.value = 0;
    let message = `OTA failed with HTTP ${xhr.status}.`;
    try {
      const data = JSON.parse(xhr.responseText || "{}");
      message = data.error || data.message || message;
    } catch {
      if (xhr.responseText) message = xhr.responseText;
    }
    setStatus("ota-status", message, "error");
    showToast(message, "error");
  };
  xhr.onerror = () => {
    button.disabled = false;
    progress.value = 0;
    const message = "OTA upload connection dropped. Stop BLE scanning, keep this page open, and retry near the router.";
    setStatus("ota-status", message, "error");
    showToast(message, "error");
  };
  xhr.send(file);
});

loadGattSnapshots();
applyI18n();
renderGattInspection();
loadStatus();
showDashboard(Boolean(apiToken));
if (!apiToken) {
  scanNetworksInto("onboarding-wifi-networks").catch(() => {});
}
if (apiToken) {
  loadWebhook().catch(() => {});
  loadMqtt().catch(() => {});
  loadBle().catch(() => {});
  loadHardware().catch(() => {});
  loadDevices().catch(() => {});
  loadBleSensors().catch(() => {});
  loadCloudOta().catch(() => {});
  loadLogs().catch(() => showDashboard(false));
  scanNetworks().catch(() => {
    $("wifi-networks").innerHTML = '<option value="">Scan failed</option>';
  });
}
setInterval(loadStatus, 3000);
setInterval(() => {
  if (apiToken) loadLogs().catch(() => {});
}, 10000);
setInterval(() => {
  if (apiToken && !webhookFormDirty) loadWebhook().catch(() => {});
}, 10000);
setInterval(() => {
  if (apiToken) loadMqtt().catch(() => {});
}, 5000);
setInterval(() => {
  if (apiToken) loadBle().catch(() => {});
}, 5000);
setInterval(() => {
  if (apiToken) loadHardware().catch(() => {});
}, 15000);
setInterval(() => {
  if (apiToken) loadBleSensors().catch(() => {});
}, 7000);
setInterval(() => {
  if (apiToken) loadDevices().catch(() => {});
}, 10000);
setInterval(() => {
  if (apiToken) loadCloudOta().catch(() => {});
}, 7000);
