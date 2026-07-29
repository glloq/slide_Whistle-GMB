/*
 * api.js — the ONLY way the UI talks to /api/v1.
 *
 * Every helper throws on a non-2xx response or an {ok:false} body, so a caller
 * can never mistake an HTTP 400/500 for success (corrections #16/#23). The
 * error carries the firmware's {code,message,field} for precise UI feedback.
 */

export class ApiError extends Error {
  constructor(status, code, message, field) {
    super(message || code || `HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.code = code || "";
    this.field = field || "";
  }
}

// `fetchImpl` is injectable so this module is unit-tested under node.
export function makeApi({ fetchImpl = fetch, getToken = () => "", baseUrl = "" } = {}) {
  async function request(method, path, body) {
    const headers = {};
    const token = getToken();
    if (token) headers["X-Auth-Token"] = token;
    let payload;
    if (body !== undefined) {
      headers["Content-Type"] = "application/json";
      payload = JSON.stringify(body);
    }
    const res = await fetchImpl(baseUrl + path, { method, headers, body: payload });

    // Parse JSON defensively; a non-JSON body on an error is still an error.
    let data = null;
    const raw = await safeText(res);
    if (raw) { try { data = JSON.parse(raw); } catch { data = null; } }

    if (!res.ok) {
      const e = (data && data.error) || {};
      throw new ApiError(res.status, e.code, e.message || raw, e.field);
    }
    if (data && data.ok === false) {
      const e = data.error || {};
      throw new ApiError(res.status, e.code, e.message, e.field);
    }
    return data ? (data.data ?? data) : {};
  }

  return {
    get: (p) => request("GET", p),
    post: (p, b) => request("POST", p, b ?? {}),
    // convenience wrappers
    status: () => request("GET", "/api/v1/status"),
    getConfig: () => request("GET", "/api/v1/config"),
    putConfig: (cfg) => request("POST", "/api/v1/config", cfg),
    applyPreset: (index, instrument) => request("POST", "/api/v1/preset", { index, instrument }),
    login: (token) => request("POST", "/api/v1/session", { token }),
    command: (cmd) => request("POST", "/api/v1/command", cmd),
    factoryReset: () => request("POST", "/api/v1/factory-reset", {}),
    restart: () => request("POST", "/api/v1/restart", {}),
    request,
  };
}

async function safeText(res) {
  try { return await res.text(); } catch { return ""; }
}
