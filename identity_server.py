#!/usr/bin/env python3
"""
Servicio de Identidad - DCGP
Servidor HTTP básico para autenticación de usuarios.
Separado del servidor de juego (requisito del proyecto).

Uso: python3 identity_server.py [puerto]
"""

import json
import hashlib
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import sys
import logging

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler("identity.log")
    ]
)
log = logging.getLogger("identity")

# ---------------------------------------------------------------------------
# "Base de datos" de usuarios (en producción sería una BD real)
# ---------------------------------------------------------------------------
USERS = {
    "alice":   {"password": hashlib.sha256(b"pass123").hexdigest(), "role": "ATTACKER"},
    "bob":     {"password": hashlib.sha256(b"pass456").hexdigest(), "role": "DEFENDER"},
    "charlie": {"password": hashlib.sha256(b"pass789").hexdigest(), "role": "ATTACKER"},
    "diana":   {"password": hashlib.sha256(b"pass000").hexdigest(), "role": "DEFENDER"},
    "eve":     {"password": hashlib.sha256(b"eve2026").hexdigest(), "role": "ATTACKER"},
    "frank":   {"password": hashlib.sha256(b"frank26").hexdigest(), "role": "DEFENDER"},
}

class IdentityHandler(BaseHTTPRequestHandler):
    server_version = "DCGP-Identity/1.0"

    def log_message(self, format, *args):
        log.info(f"[{self.client_address[0]}:{self.client_address[1]}] {format % args}")

    def send_json(self, code: int, data: dict):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)

        # GET /health
        if parsed.path == "/health":
            self.send_json(200, {"status": "ok", "service": "identity"})
            return

        # GET /user?username=<u>
        if parsed.path == "/user":
            username = params.get("username", [None])[0]
            if not username:
                self.send_json(400, {"error": "missing username"})
                return
            user = USERS.get(username)
            if not user:
                self.send_json(404, {"error": "user not found"})
                return
            # No devolver contraseña
            self.send_json(200, {"username": username, "role": user["role"]})
            return

        self.send_json(404, {"error": "endpoint not found"})

    def do_POST(self):
        parsed = urlparse(self.path)

        # POST /auth  ← body: {"username": "...", "password": "..."}
        if parsed.path == "/auth":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body   = self.rfile.read(length)
                data   = json.loads(body)
            except Exception as e:
                self.send_json(400, {"error": f"invalid body: {e}"})
                return

            username = data.get("username", "")
            password = data.get("password", "")

            if not username or not password:
                self.send_json(400, {"error": "missing credentials"})
                return

            user = USERS.get(username)
            if not user:
                self.send_json(401, {"error": "invalid credentials"})
                return

            pw_hash = hashlib.sha256(password.encode()).hexdigest()
            if pw_hash != user["password"]:
                self.send_json(401, {"error": "invalid credentials"})
                return

            log.info(f"AUTH OK: {username} ({user['role']})")
            self.send_json(200, {
                "username": username,
                "role":     user["role"],
                "status":   "authenticated"
            })
            return

        self.send_json(404, {"error": "endpoint not found"})

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
    server = HTTPServer(("0.0.0.0", port), IdentityHandler)
    log.info(f"Identity Server escuchando en puerto {port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Servidor detenido.")
