#!/usr/bin/env python3
"""
Servidor HTTP Web - DCGP
Interfaz web para login y listado de partidas.
Se comunica con el Identity Server y con el Game Server.

Uso: python3 web_server.py [puerto]
"""

import json
import socket
import requests
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import sys
import os
import logging

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(message)s",
    handlers=[logging.StreamHandler(), logging.FileHandler("web.log")]
)
log = logging.getLogger("web")

# ---------------------------------------------------------------------------
# Resolución de nombres — NO se usan IPs hardcodeadas
# ---------------------------------------------------------------------------
def resolve_host(hostname: str) -> str:
    try:
        return socket.gethostbyname(hostname)
    except socket.gaierror as e:
        log.error(f"DNS resolution failed for {hostname}: {e}")
        return None

IDENTITY_HOST = os.environ.get("IDENTITY_HOST", "localhost")
IDENTITY_PORT = int(os.environ.get("IDENTITY_PORT", "8081"))
GAME_HOST     = os.environ.get("GAME_HOST",     "localhost")
GAME_PORT     = int(os.environ.get("GAME_PORT",     "8080"))

HTML_PAGE = """<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DCGP – Datacenter Game</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@300;600;700&display=swap');
  :root {
    --bg: #080c10; --surface: #0d1520; --border: #1a3a4a;
    --accent: #00e5ff; --warn: #ff4444; --ok: #00ff88; --text: #c8d8e8;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: 'Rajdhani', sans-serif; min-height: 100vh;
    display: flex; flex-direction: column; align-items: center;
    justify-content: flex-start; padding: 2rem;
    background-image:
      radial-gradient(ellipse at 20% 20%, rgba(0,229,255,.06) 0%, transparent 60%),
      radial-gradient(ellipse at 80% 80%, rgba(0,255,136,.04) 0%, transparent 60%);
  }
  header { width: 100%; max-width: 600px; text-align: center; margin-bottom: 2.5rem; }
  header h1 {
    font-size: 2.4rem; font-weight: 700; letter-spacing: .15em;
    color: var(--accent); text-transform: uppercase;
    text-shadow: 0 0 24px rgba(0,229,255,.5);
  }
  header p { color: #5a7a8a; font-size: .95rem; letter-spacing: .08em; margin-top: .3rem; }
  .card {
    width: 100%; max-width: 480px; background: var(--surface);
    border: 1px solid var(--border); border-radius: 6px;
    padding: 2rem; margin-bottom: 1.5rem;
  }
  .card h2 {
    font-size: 1rem; font-weight: 600; letter-spacing: .1em;
    text-transform: uppercase; color: var(--accent);
    margin-bottom: 1.2rem; padding-bottom: .5rem;
    border-bottom: 1px solid var(--border);
  }
  label { display: block; font-size: .85rem; letter-spacing: .06em; color: #7a9aaa; margin-bottom: .3rem; }
  input {
    width: 100%; padding: .65rem .9rem; background: #060a0e;
    border: 1px solid var(--border); border-radius: 4px; color: var(--text);
    font-family: 'Share Tech Mono', monospace; font-size: .9rem;
    margin-bottom: 1rem; outline: none; transition: border-color .2s;
  }
  input:focus { border-color: var(--accent); }
  button {
    width: 100%; padding: .75rem; background: transparent;
    border: 1px solid var(--accent); color: var(--accent);
    font-family: 'Rajdhani', sans-serif; font-size: 1rem;
    font-weight: 700; letter-spacing: .12em; text-transform: uppercase;
    cursor: pointer; border-radius: 4px; transition: background .2s, color .2s;
  }
  button:hover { background: var(--accent); color: var(--bg); }
  #msg { margin-top: .8rem; font-size: .9rem; min-height: 1.2rem; }
  #msg.ok  { color: var(--ok); }
  #msg.err { color: var(--warn); }
  #rooms-section { display: none; }
  .room-list { list-style: none; }
  .room-list li {
    display: flex; align-items: center; justify-content: space-between;
    padding: .6rem .9rem; margin-bottom: .5rem;
    background: #060a0e; border: 1px solid var(--border); border-radius: 4px;
    font-family: 'Share Tech Mono', monospace; font-size: .85rem;
  }
  .room-list li button { width: auto; padding: .3rem .9rem; font-size: .8rem; }
  .tag-att { color: var(--warn); }
  .tag-def { color: var(--ok); }
  footer { color: #2a4a5a; font-size: .75rem; margin-top: auto; padding-top: 2rem; }
</style>
</head>
<body>
<header>
  <h1>⬡ DCGP</h1>
  <p>Datacenter Guard Protocol &mdash; EAFIT 2026-1</p>
</header>

<div class="card" id="login-card">
  <h2>Autenticación</h2>
  <label>Usuario</label>
  <input type="text" id="username" placeholder="alice" autocomplete="off">
  <label>Contraseña</label>
  <input type="password" id="password" placeholder="••••••••">
  <button onclick="login()">Ingresar</button>
  <div id="msg"></div>
</div>

<div class="card" id="rooms-section">
  <h2>Partidas activas <span id="user-badge"></span></h2>
  <ul class="room-list" id="room-list"><li>Cargando...</li></ul>
  <button onclick="loadRooms()" style="margin-top:.5rem">↺ Actualizar</button>
</div>

<footer>Internet: Arquitectura y Protocolos &mdash; Todos los derechos reservados</footer>

<script>
let currentUser = null;

async function login() {
  const u = document.getElementById('username').value.trim();
  const p = document.getElementById('password').value;
  const msg = document.getElementById('msg');
  msg.className = ''; msg.textContent = 'Verificando...';

  try {
    const r = await fetch('/api/auth', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({username: u, password: p})
    });
    const data = await r.json();
    if (!r.ok) {
      msg.className = 'err';
      msg.textContent = data.error || 'Error de autenticación';
      return;
    }
    currentUser = data;
    msg.className = 'ok';
    msg.textContent = `✔ Bienvenido, ${data.username} (${data.role})`;
    document.getElementById('user-badge').innerHTML =
      `<span class="${data.role==='ATTACKER'?'tag-att':'tag-def'}">${data.role}</span>`;
    document.getElementById('rooms-section').style.display = 'block';
    loadRooms();
  } catch(e) {
    msg.className = 'err';
    msg.textContent = 'No se pudo conectar al servidor.';
  }
}

async function loadRooms() {
  const ul = document.getElementById('room-list');
  ul.innerHTML = '<li>Cargando...</li>';
  try {
    const r = await fetch('/api/rooms');
    const data = await r.json();
    ul.innerHTML = '';
    if (!data.rooms || data.rooms.length === 0) {
      ul.innerHTML = '<li style="color:#5a7a8a">Sin partidas activas</li>';
    } else {
      data.rooms.forEach(room => {
        const li = document.createElement('li');
        li.innerHTML = `<span>Sala <b>#${room.id}</b> — ${room.players} jugador(es)</span>
          <button onclick="joinRoom(${room.id})">UNIRSE</button>`;
        ul.appendChild(li);
      });
    }
    // Opción de crear nueva sala
    const li = document.createElement('li');
    li.innerHTML = `<span style="color:#5a7a8a">Nueva sala</span>
      <button onclick="joinRoom('NEW')">CREAR</button>`;
    ul.appendChild(li);
  } catch(e) {
    ul.innerHTML = '<li style="color:#ff4444">Error al obtener salas.</li>';
  }
}

async function joinRoom(roomId) {
  if (!currentUser) return;
  const clientType = currentUser.role === 'ATTACKER'
    ? 'python3 client_attacker.py'
    : 'java DefenderClient';
  const info = `Lanza el cliente de juego:\n\n`
    + `  ${clientType}\n\n`
    + `Servidor: ${location.hostname}:8080\n`
    + `Usuario:  ${currentUser.username}\n`
    + `Rol:      ${currentUser.role}\n`
    + `Sala:     ${roomId}`;
  alert(info);
}

document.addEventListener('keydown', e => { if (e.key === 'Enter') login(); });
</script>
</body>
</html>
"""

class WebHandler(BaseHTTPRequestHandler):
    server_version = "DCGP-Web/1.0"

    def log_message(self, format, *args):
        log.info(f"[{self.client_address[0]}:{self.client_address[1]}] "
                 f"{self.command} {self.path} -> {format % args}")

    def send_json(self, code, data):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, code, html):
        body = html.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path in ("/", "/index.html"):
            self.send_html(200, HTML_PAGE)
            return

        if parsed.path == "/api/rooms":
            self._get_rooms()
            return

        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/auth":
            self._post_auth()
            return
        self.send_json(404, {"error": "not found"})

    def _post_auth(self):
        """Redirige la autenticación al identity server. Sin IPs hardcodeadas."""
        try:
            length = int(self.headers.get("Content-Length", 0))
            body   = self.rfile.read(length)
            data   = json.loads(body)
        except Exception as e:
            self.send_json(400, {"error": f"bad request: {e}"})
            return

        ip = resolve_host(IDENTITY_HOST)
        if not ip:
            self.send_json(503, {"error": "identity service unavailable"})
            return

        try:
            r = requests.post(
                f"http://{IDENTITY_HOST}:{IDENTITY_PORT}/auth",
                json=data, timeout=5
            )
            self.send_json(r.status_code, r.json())
        except Exception as e:
            log.error(f"Identity server error: {e}")
            self.send_json(503, {"error": "identity service error"})

    def _get_rooms(self):
        """
        Consulta salas activas al game server vía TCP.
        FIX: ya no autentica como usuario fantasma (__web__ DEFENDER).
        LIST no requiere AUTH en el protocolo DCGP, así que se consulta directamente.
        """
        try:
            ip = resolve_host(GAME_HOST)
            if not ip:
                self.send_json(503, {"error": "game server unavailable"})
                return

            s = socket.create_connection((GAME_HOST, GAME_PORT), timeout=3)
            s.recv(256)                      # leer WELCOME
            s.sendall(b"LIST\r\n")           # LIST no requiere AUTH
            resp = s.recv(1024).decode(errors="ignore").strip()
            s.sendall(b"QUIT\r\n")
            s.close()

            rooms = []
            if resp.startswith("ROOMS"):
                parts = resp.split()[1:]
                for p in parts:
                    if p == "EMPTY":
                        continue
                    try:
                        rid, cnt = p.split(":")
                        rooms.append({"id": int(rid), "players": int(cnt)})
                    except ValueError:
                        pass

            self.send_json(200, {"rooms": rooms})
        except Exception as e:
            log.error(f"Game server query error: {e}")
            self.send_json(200, {"rooms": []})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8082
    httpd = HTTPServer(("0.0.0.0", port), WebHandler)
    log.info(f"Web Server escuchando en puerto {port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log.info("Web server detenido.")
