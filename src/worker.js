const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "*",
  "Access-Control-Max-Age": "86400"
};

const R2_PUBLIC_BASE = "https://pub-5dfb9ff28fd04cd1960cdb8b34e19dbd.r2.dev";
const WORKER_BASE_URL = "https://waterlevelmonitor.luumanhkien08092006.workers.dev";

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    if (request.method === "POST" && path === "/alert") return handleAlert(request, env, ctx);
    if (request.method === "POST" && path === "/log") return handleLog(request, env);
    if (request.method === "POST" && path === "/setting-log") return handleSettingLog(request, env);
    if (request.method === "GET" && path === "/get-settings") return handleGetSettings(request, env);
    if (request.method === "GET" && path === "/export") return handleExport(request, env);
    if (request.method === "GET" && path === "/history-data") return handleHistoryData(request, env);
    if (request.method === "POST" && path === "/clear-data") return handleClearData(request, env);

    if (request.method === "POST" && path === "/ota/upload") return handleOtaUpload(request, env);
    if (request.method === "GET" && path === "/ota/list") return handleOtaList(request, env);
    if (request.method === "POST" && path === "/ota/activate") return handleOtaActivate(request, env);
    if (request.method === "POST" && path === "/ota/deactivate") return handleOtaDeactivate(request, env);
    if (request.method === "POST" && path === "/ota/delete") return handleOtaDelete(request, env);
    if (request.method === "GET" && path === "/ota/active") return handleOtaActive(request, env);
    if (request.method === "POST" && path === "/ota/checkin") return handleOtaCheckin(request, env);
    if (request.method === "GET" && path === "/ota/checkin-status") return handleOtaCheckinStatus(request, env);
    if (request.method === "GET" && path === "/ota/download") return handleOtaDownload(request, env);

    return new Response("Not found", { status: 404, headers: CORS_HEADERS });
  }
};

const VALID_LEVELS = ["danger", "warn", "detect", "safe", "error"];

function checkKey(request, env) {
  const deviceKey = request.headers.get("X-Device-Key");
  return deviceKey && deviceKey === env.DEVICE_KEY;
}

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json", ...CORS_HEADERS }
  });
}

async function handleAlert(request, env, ctx) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { distance, level, deviceId } = body;
  if (typeof distance !== "number" || !VALID_LEVELS.includes(level)) {
    return new Response("Missing or invalid fields", { status: 400, headers: CORS_HEADERS });
  }

  const id = deviceId || "default";
  const stateKey = `last_level:${id}`;

  const prevLevel = await env.ALERT_STATE.get(stateKey);
  if (prevLevel === level) {
    return json({ sent: false, reason: "no state change" });
  }

  ctx.waitUntil(env.ALERT_STATE.put(stateKey, level));

  if (level === "safe") {
    return json({ sent: false, reason: "safe state" });
  }

  let text;
  if (level === "detect") {
    text = "Phát hiện nước trong hầm.";
  } else if (level === "warn") {
    text = "Mực nước không an toàn, nhanh chóng sơ tán người và phương tiện nếu có thể";
  } else if (level === "danger") {
    text = "Mực nước nguy hiểm, ưu tiên sơ tán khẩn cấp";
  } else if (level === "error") {
    text = "Cảm biến gặp lỗi, không đọc được dữ liệu.";
  }

  const tgUrl = `https://api.telegram.org/bot${env.TELEGRAM_BOT_TOKEN}/sendMessage`;
  ctx.waitUntil(
    fetch(tgUrl, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ chat_id: env.TELEGRAM_CHAT_ID, text })
    })
  );

  return json({ sent: true });
}

async function handleLog(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { distance, level, rate, deviceId } = body;
  if (typeof distance !== "number" || !level) {
    return new Response("Missing fields", { status: 400, headers: CORS_HEADERS });
  }

  const now = Math.floor(Date.now() / 1000);
  const cutoff = now - 24 * 60 * 60;

  await env.WATERLEVEL.batch([
    env.WATERLEVEL.prepare(
      "INSERT INTO readings (device_id, epoch, distance, rate, level, received_at) VALUES (?, ?, ?, ?, ?, ?)"
    ).bind(deviceId || "default", now, distance, rate || 0, level, now),
    env.WATERLEVEL.prepare(
      "DELETE FROM readings WHERE epoch < ?"
    ).bind(cutoff)
  ]);

  return json({ logged: true });
}

const SENSOR_HEIGHT_MIN = 1;
const SENSOR_HEIGHT_MAX = 400;

async function handleSettingLog(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { danger, warn, detect, sensorHeight, deviceId } = body;
  if (typeof danger !== "number" || typeof warn !== "number" || typeof detect !== "number") {
    return new Response("Missing fields", { status: 400, headers: CORS_HEADERS });
  }

  let heightToStore = null;
  if (sensorHeight !== undefined && sensorHeight !== null) {
    if (typeof sensorHeight !== "number" || sensorHeight < SENSOR_HEIGHT_MIN || sensorHeight > SENSOR_HEIGHT_MAX) {
      return new Response("Invalid sensorHeight", { status: 400, headers: CORS_HEADERS });
    }
    heightToStore = sensorHeight;
  } else {
    const prev = await env.WATERLEVEL.prepare(
      "SELECT sensor_height FROM settings_log WHERE device_id = ? AND sensor_height IS NOT NULL ORDER BY ts DESC LIMIT 1"
    ).bind(deviceId || "default").first();
    heightToStore = prev ? prev.sensor_height : null;
  }

  await env.WATERLEVEL.prepare(
    "INSERT INTO settings_log (device_id, danger_threshold, warn_threshold, detect_threshold, sensor_height, ts) VALUES (?, ?, ?, ?, ?, ?)"
  ).bind(deviceId || "default", danger, warn, detect, heightToStore, Date.now()).run();

  return json({ logged: true });
}

function otaInfo(row, deviceId) {
  if (!row) return { active: false };
  return {
    active: true,
    id: row.id,
    version: row.version,
    size: row.size,
    url: `${WORKER_BASE_URL}/ota/download?deviceId=${encodeURIComponent(deviceId)}`
  };
}

async function handleGetSettings(request, env) {
  const url = new URL(request.url);
  const deviceId = url.searchParams.get("deviceId") || "default";

  const [settingsRes, otaRes] = await env.WATERLEVEL.batch([
    env.WATERLEVEL.prepare(
      "SELECT danger_threshold, warn_threshold, detect_threshold, sensor_height, ts FROM settings_log WHERE device_id = ? ORDER BY ts DESC LIMIT 1"
    ).bind(deviceId),
    env.WATERLEVEL.prepare(
      "SELECT id, version, size FROM ota_versions WHERE device_id = ? AND is_active = 1 LIMIT 1"
    ).bind(deviceId)
  ]);

  const row = settingsRes.results[0];
  return json({
    danger: row ? row.danger_threshold : null,
    warn: row ? row.warn_threshold : null,
    detect: row ? row.detect_threshold : null,
    sensorHeight: row ? row.sensor_height : null,
    ts: row ? row.ts : null,
    ota: otaInfo(otaRes.results[0], deviceId)
  });
}

async function handleExport(request, env) {
  const { results } = await env.WATERLEVEL.prepare(
    "SELECT device_id, distance, level, rate, epoch FROM readings ORDER BY epoch DESC LIMIT 2000"
  ).all();

  let csv = "\uFEFF";
  csv += "device_id,distance_cm,level,rate_cm_per_min,timestamp\n";
  for (const row of results) {
    const date = new Date(row.epoch * 1000).toLocaleString("vi-VN");
    csv += `${row.device_id},${row.distance},${row.level},${row.rate},${date}\n`;
  }

  return new Response(csv, {
    headers: {
      "Content-Type": "text/csv; charset=utf-8",
      "Content-Disposition": "attachment; filename=water_readings.csv",
      ...CORS_HEADERS
    }
  });
}

async function handleHistoryData(request, env) {
  const { results } = await env.WATERLEVEL.prepare(
    "SELECT device_id, distance, level, rate, epoch FROM readings ORDER BY epoch DESC LIMIT 100"
  ).all();

  return json(results);
}

async function handleClearData(request, env) {
  if (!checkKey(request, env)) {
    return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });
  }

  let body;
  try {
    body = await request.json();
  } catch (e) {
    body = {};
  }

  const { scope, deviceId } = body;

  if (scope === "device") {
    if (!deviceId) {
      return new Response("Missing deviceId", { status: 400, headers: CORS_HEADERS });
    }
    await env.WATERLEVEL.prepare(
      "DELETE FROM readings WHERE device_id = ?"
    ).bind(deviceId).run();
  } else {
    await env.WATERLEVEL.prepare("DELETE FROM readings").run();
  }

  return json({ cleared: true });
}

async function handleOtaUpload(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  const url = new URL(request.url);
  const version = url.searchParams.get("version");
  const note = url.searchParams.get("note") || "";
  const deviceId = url.searchParams.get("deviceId") || "default";

  if (!version) return new Response("Missing version", { status: 400, headers: CORS_HEADERS });

  const bodyBuffer = await request.arrayBuffer();
  if (!bodyBuffer || bodyBuffer.byteLength === 0) {
    return new Response("Empty file", { status: 400, headers: CORS_HEADERS });
  }
  const firstByte = new Uint8Array(bodyBuffer, 0, 1)[0];
  if (firstByte !== 0xe9) {
    return new Response("File không phải firmware .bin hợp lệ (thiếu magic byte 0xE9)", {
      status: 400,
      headers: CORS_HEADERS
    });
  }

  const ts = Date.now();
  const key = `ota/${deviceId}_${ts}_${version}.bin`;

  await env.OTA_BUCKET.put(key, bodyBuffer, {
    httpMetadata: { contentType: "application/octet-stream" }
  });

  await env.WATERLEVEL.prepare(
    "INSERT INTO ota_versions (device_id, version, note, filename, size, uploaded_at, is_active) VALUES (?, ?, ?, ?, ?, ?, 0)"
  ).bind(deviceId, version, note, key, bodyBuffer.byteLength, ts).run();

  return json({ uploaded: true, filename: key, size: bodyBuffer.byteLength });
}

async function handleOtaList(request, env) {
  const url = new URL(request.url);
  const deviceId = url.searchParams.get("deviceId") || "default";

  const { results } = await env.WATERLEVEL.prepare(
    "SELECT id, version, note, filename, size, uploaded_at, is_active FROM ota_versions WHERE device_id = ? ORDER BY uploaded_at DESC"
  ).bind(deviceId).all();

  const list = results.map(r => ({
    ...r,
    url: `${R2_PUBLIC_BASE}/${r.filename}`
  }));

  return json(list);
}

async function handleOtaActivate(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { id, deviceId } = body;
  if (!id) return new Response("Missing id", { status: 400, headers: CORS_HEADERS });
  const devId = deviceId || "default";

  await env.WATERLEVEL.batch([
    env.WATERLEVEL.prepare("UPDATE ota_versions SET is_active = 0 WHERE device_id = ?").bind(devId),
    env.WATERLEVEL.prepare("UPDATE ota_versions SET is_active = 1 WHERE id = ? AND device_id = ?").bind(id, devId)
  ]);

  await env.ALERT_STATE.put(`ota_checkin:${devId}`, JSON.stringify({ status: "pending", ts: Date.now() }));

  return json({ activated: true });
}

async function handleOtaDeactivate(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const devId = body.deviceId || "default";

  await env.WATERLEVEL.prepare(
    "UPDATE ota_versions SET is_active = 0 WHERE device_id = ?"
  ).bind(devId).run();

  await env.ALERT_STATE.put(`ota_checkin:${devId}`, JSON.stringify({ status: "deactivated", ts: Date.now() }));

  return json({ deactivated: true });
}

async function handleOtaDelete(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { id, deviceId, force } = body;
  if (!id) return new Response("Missing id", { status: 400, headers: CORS_HEADERS });
  const devId = deviceId || "default";

  const row = await env.WATERLEVEL.prepare(
    "SELECT filename, is_active FROM ota_versions WHERE id = ? AND device_id = ?"
  ).bind(id, devId).first();

  if (!row) return new Response("Not found", { status: 404, headers: CORS_HEADERS });

  if (row.is_active && !force) {
    return new Response("Không thể xoá bản đang active, hãy activate bản khác trước (hoặc gửi force:true để ngừng active rồi xoá luôn)", {
      status: 400,
      headers: CORS_HEADERS
    });
  }

  if (row.is_active && force) {
    await env.WATERLEVEL.prepare("UPDATE ota_versions SET is_active = 0 WHERE id = ?").bind(id).run();
  }

  await env.OTA_BUCKET.delete(row.filename);
  await env.WATERLEVEL.prepare("DELETE FROM ota_versions WHERE id = ?").bind(id).run();

  return json({ deleted: true });
}

async function handleOtaActive(request, env) {
  const url = new URL(request.url);
  const deviceId = url.searchParams.get("deviceId") || "default";

  const row = await env.WATERLEVEL.prepare(
    "SELECT id, version, size FROM ota_versions WHERE device_id = ? AND is_active = 1 LIMIT 1"
  ).bind(deviceId).first();

  return json(otaInfo(row, deviceId));
}

async function handleOtaDownload(request, env) {
  const url = new URL(request.url);
  const deviceId = url.searchParams.get("deviceId") || "default";

  const row = await env.WATERLEVEL.prepare(
    "SELECT filename, size FROM ota_versions WHERE device_id = ? AND is_active = 1 LIMIT 1"
  ).bind(deviceId).first();

  if (!row) return new Response("No active version", { status: 404, headers: CORS_HEADERS });

  const object = await env.OTA_BUCKET.get(row.filename);
  if (!object) return new Response("File missing in R2", { status: 404, headers: CORS_HEADERS });

  return new Response(object.body, {
    headers: {
      "Content-Type": "application/octet-stream",
      "Content-Length": String(row.size),
      ...CORS_HEADERS
    }
  });
}

async function handleOtaCheckin(request, env) {
  if (!checkKey(request, env)) return new Response("Unauthorized", { status: 401, headers: CORS_HEADERS });

  let body;
  try {
    body = await request.json();
  } catch (e) {
    body = {};
  }
  const deviceId = body.deviceId || "default";
  const version = body.version || "unknown";
  const ok = body.ok !== false;
  const error = typeof body.error === "string" ? body.error : "";

  const record = { status: ok ? "ok" : "failed", version, ts: Date.now() };
  if (!ok && error) record.error = error;

  await env.ALERT_STATE.put(`ota_checkin:${deviceId}`, JSON.stringify(record));

  return json({ recorded: true });
}

async function handleOtaCheckinStatus(request, env) {
  const url = new URL(request.url);
  const deviceId = url.searchParams.get("deviceId") || "default";

  const raw = await env.ALERT_STATE.get(`ota_checkin:${deviceId}`);
  if (!raw) return json({ status: "none" });

  let record;
  try {
    record = JSON.parse(raw);
  } catch (e) {
    return json({ status: "none" });
  }

  return json(record);
}