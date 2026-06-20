#!/usr/bin/env node
"use strict";

/*
  WT02E40E BLE + Wi-Fi Console Server

  Built-in modules only, no npm install needed.

  What it does:
    - Serves index.html on http://localhost:8080
    - Opens a UDP listener for WT02E40E Wi-Fi messages, default port 5000
    - Streams UDP packets into the browser with Server-Sent Events
    - Can optionally send UDP packets back out from the browser

  Usage:
    node server.js
    node server.js --http 8080 --udp 5000
*/

const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");
const dgram = require("dgram");
const { URL } = require("url");

const args = parseArgs(process.argv.slice(2));

const HTTP_PORT = numberArg(args.http, 8080);
const UDP_PORT = numberArg(args.udp, 5000);
const HOST = args.host || "0.0.0.0";
const ROOT = __dirname;
const WEBAPP_VERSION = "2026-06-20-read-status-lock-v3";

const clients = new Set();
const udpLog = [];

const udp = dgram.createSocket("udp4");

udp.on("error", err => {
  console.error("[udp] error:", err);
});

udp.on("message", (msg, rinfo) => {
  const item = {
    type: "udp_rx",
    time: new Date().toISOString(),
    from: `${rinfo.address}:${rinfo.port}`,
    address: rinfo.address,
    port: rinfo.port,
    length: msg.length,
    text: msg.toString("utf8"),
    hex: msg.toString("hex")
  };

  udpLog.push(item);
  while (udpLog.length > 500) {
    udpLog.shift();
  }

  console.log(`[udp] <= ${item.from} ${JSON.stringify(item.text)}`);
  broadcast(item);
});

udp.bind(UDP_PORT, HOST, () => {
  const address = udp.address();
  console.log(`[udp] listening on ${address.address}:${address.port}`);
});

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);

    if (req.method === "GET" && url.pathname === "/events") {
      handleEvents(req, res);
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/config") {
      sendJson(res, {
        httpPort: HTTP_PORT,
        udpPort: UDP_PORT,
        host: HOST,
        addresses: getLanAddresses(),
        version: WEBAPP_VERSION
      });
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/udp/log") {
      sendJson(res, udpLog);
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/udp/send") {
      const body = await readJson(req);
      const host = String(body.host || "").trim();
      const port = Number(body.port);
      const text = String(body.text || "");

      if (!host || !Number.isInteger(port) || port < 1 || port > 65535) {
        sendJson(res, { ok: false, error: "host and valid port are required" }, 400);
        return;
      }

      const payload = Buffer.from(text, "utf8");
      udp.send(payload, port, host, err => {
        if (err) {
          sendJson(res, { ok: false, error: err.message }, 500);
          return;
        }

        const item = {
          type: "udp_tx",
          time: new Date().toISOString(),
          to: `${host}:${port}`,
          host,
          port,
          length: payload.length,
          text,
          hex: payload.toString("hex")
        };

        console.log(`[udp] => ${item.to} ${JSON.stringify(text)}`);
        broadcast(item);
        sendJson(res, { ok: true, sent: item });
      });

      return;
    }

    if (req.method === "POST" && url.pathname === "/api/udp/clear") {
      udpLog.length = 0;
      broadcast({ type: "udp_clear", time: new Date().toISOString() });
      sendJson(res, { ok: true });
      return;
    }

    serveStatic(url.pathname, res);
  } catch (err) {
    console.error("[http] error:", err);
    sendText(res, "Internal server error\n", 500, "text/plain");
  }
});

server.listen(HTTP_PORT, HOST, () => {
  console.log(`[http] WT02E40E BLE_Webapp ${WEBAPP_VERSION}`);
  console.log(`[http] listening on http://localhost:${HTTP_PORT}`);
  const addresses = getLanAddresses();
  for (const address of addresses) {
    console.log(`[http] LAN URL http://${address}:${HTTP_PORT}`);
    console.log(`[udp]  WT command target ${address} ${UDP_PORT}`);
  }
});

function handleEvents(req, res) {
  res.writeHead(200, {
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    "Connection": "keep-alive",
    "X-Accel-Buffering": "no"
  });

  const client = { res };
  clients.add(client);

  sendEvent(res, {
    type: "server_connected",
    time: new Date().toISOString(),
    udpPort: UDP_PORT,
    addresses: getLanAddresses()
  });

  const keepAlive = setInterval(() => {
    res.write(": keepalive\n\n");
  }, 15000);

  req.on("close", () => {
    clearInterval(keepAlive);
    clients.delete(client);
  });
}

function broadcast(obj) {
  for (const client of clients) {
    sendEvent(client.res, obj);
  }
}

function sendEvent(res, obj) {
  res.write(`data: ${JSON.stringify(obj)}\n\n`);
}

function serveStatic(requestPath, res) {
  const safePath = requestPath === "/" ? "/index.html" : requestPath;
  const filePath = path.normalize(path.join(ROOT, safePath));

  if (!filePath.startsWith(ROOT)) {
    sendText(res, "Forbidden\n", 403, "text/plain");
    return;
  }

  fs.readFile(filePath, (err, data) => {
    if (err) {
      sendText(res, "Not found\n", 404, "text/plain");
      return;
    }

    sendBytes(res, data, 200, contentType(filePath));
  });
}

function sendJson(res, obj, status = 200) {
  sendText(res, JSON.stringify(obj, null, 2), status, "application/json");
}

function sendText(res, text, status = 200, type = "text/plain") {
  sendBytes(res, Buffer.from(text, "utf8"), status, `${type}; charset=utf-8`);
}

function sendBytes(res, bytes, status, type) {
  res.writeHead(status, {
    "Content-Type": type,
    "Content-Length": bytes.length,
    "Access-Control-Allow-Origin": "*",
    "Cache-Control": "no-store, max-age=0",
    "X-WT02E40E-Webapp": WEBAPP_VERSION
  });
  res.end(bytes);
}

function contentType(filePath) {
  const ext = path.extname(filePath).toLowerCase();

  switch (ext) {
    case ".html": return "text/html; charset=utf-8";
    case ".css": return "text/css; charset=utf-8";
    case ".js": return "application/javascript; charset=utf-8";
    case ".json": return "application/json; charset=utf-8";
    case ".svg": return "image/svg+xml";
    case ".png": return "image/png";
    case ".ico": return "image/x-icon";
    default: return "application/octet-stream";
  }
}

function readJson(req) {
  return new Promise((resolve, reject) => {
    let body = "";

    req.on("data", chunk => {
      body += chunk;
      if (body.length > 1024 * 1024) {
        req.destroy();
        reject(new Error("request body too large"));
      }
    });

    req.on("end", () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (err) {
        reject(err);
      }
    });

    req.on("error", reject);
  });
}

function getLanAddresses() {
  const nets = os.networkInterfaces();
  const out = [];

  for (const items of Object.values(nets)) {
    for (const item of items || []) {
      if (item.family === "IPv4" && !item.internal) {
        out.push(item.address);
      }
    }
  }

  return out;
}

function parseArgs(argv) {
  const out = {};

  for (let i = 0; i < argv.length; i += 1) {
    const item = argv[i];

    if (item.startsWith("--")) {
      const key = item.slice(2);
      const next = argv[i + 1];

      if (next && !next.startsWith("--")) {
        out[key] = next;
        i += 1;
      } else {
        out[key] = true;
      }
    }
  }

  return out;
}

function numberArg(value, fallback) {
  const num = Number(value);
  return Number.isInteger(num) && num > 0 ? num : fallback;
}
