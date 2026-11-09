"""net_tests için yerel HTTPS sunucusu. Kullanım: https_server.py <dizin> <test sürecinin pid'i>

Dizine test CA'sını (ca.pem), onunla imzalı iki sunucu sertifikasını (doğru ad: 127.0.0.1; yanlış ad:
wrong.example) ve kendinden imzalı (güvenilmeyen) bir sertifika üretir. 127.0.0.1'de rastgele portlarda üç HTTPS
sunucusu ve el sıkışmaya cevap vermeyen bir TCP portu açar; "<güvenilir> <güvenilmeyen> <sessiz> <yanlış ad>"
portlarını ready.txt'ye yazar (atomik: yarım satır okunmaz). Test süreci ölünce (öldürülse de) kendisi de çıkar;
hata server.log'a yazılır. Anahtarlar her koşuda yeniden üretilir, repoda özel anahtar tutulmaz.
"""

import http.server
import os
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback

BIG_BODY_BYTES = 1000
STALL_SECONDS = 60
PARENT_POLL_SECONDS = 0.5


def openssl(*args, cwd):
    subprocess.run(["openssl", *args], cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_certificates(work):
    key = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "1"]
    openssl("req", "-x509", *key, "-keyout", "ca.key", "-out", "ca.pem", "-subj", "/CN=noteapp test CA",
            "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            cwd=work)
    # Aynı CA'nın imzaladığı iki sertifika; yalnız adları farklı (ad doğrulaması, CURLOPT_SSL_VERIFYHOST).
    for name, alt_names in (("server", "IP:127.0.0.1,DNS:localhost"), ("wronghost", "DNS:wrong.example")):
        openssl("req", "-new", *key[:5], "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=" + name,
                cwd=work)
        with open(os.path.join(work, name + ".ext"), "w") as ext:
            ext.write("subjectAltName=%s\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n" % alt_names)
        openssl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
                "-out", name + ".pem", "-days", "1", "-extfile", name + ".ext", cwd=work)
    openssl("req", "-x509", *key, "-keyout", "untrusted.key", "-out", "untrusted.pem", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost", cwd=work)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path
        if path == "/ok":
            agents = self.headers.get_all("User-Agent", [])
            body = ("hello %s %s agents=%d" % (agents[0] if agents else "", self.headers.get("Accept", ""),
                                               len(agents))).encode()
            self.reply(200, body, [("X-RateLimit-Remaining", "42"), ("X-RateLimit-Reset", "1700000000"),
                                   ("Retry-After", "7")])
        elif path == "/limited":
            self.reply(403, b"rate limited", [("X-RateLimit-Remaining", "0"), ("X-RateLimit-Reset", "1700000123")])
        elif path == "/bad-headers":
            self.reply(200, b"x", [("X-RateLimit-Remaining", "12abc"), ("X-RateLimit-Reset", "99999999999999999999"),
                                   ("Retry-After", "Wed, 21 Oct 2015 07:28:00 GMT")])
        elif path == "/big-sized":
            self.reply(200, b"a" * BIG_BODY_BYTES)
        elif path == "/big-unsized":
            # Boyu önceden bilinmeyen gövde chunked gelir: Content-Length yok, sınırı yazma geri çağrısı keser.
            # (Bağlantı kapanışıyla biten gövde TLS'te close_notify ister; http.server onu göndermez.)
            body = b"a" * BIG_BODY_BYTES
            self.wfile.write(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                             b"%x\r\n" % len(body) + body + b"\r\n0\r\n\r\n")
        elif path == "/stall":
            self.send_response(200)
            self.send_header("Content-Length", str(BIG_BODY_BYTES))
            self.end_headers()
            self.wfile.write(b"a" * 10)
            self.wfile.flush()
            time.sleep(STALL_SECONDS)
        elif path == "/redirect-http":
            self.reply(302, b"", [("Location", "http://127.0.0.1:%d/ok" % self.server.server_address[1])])
        elif path == "/redirect-loop":
            self.reply(302, b"", [("Location", "/redirect-loop")])
        elif path == "/redirect-ok":
            # Ara yanıtın sınır başlıkları sonuca sızmamalı: değerler /ok'unkinden farklı.
            self.reply(302, b"", [("Location", "/ok"), ("Retry-After", "99"), ("X-RateLimit-Remaining", "1"),
                                  ("X-RateLimit-Reset", "111")])
        elif path.startswith("/hops/"):
            # /hops/N: N yönlendirme sonra 200.
            hops = int(path[len("/hops/"):])
            if hops > 0:
                self.reply(302, b"", [("Location", "/hops/%d" % (hops - 1))])
            else:
                self.reply(200, b"arrived")
        else:
            self.reply(404, b"not found")


def serve(work, certificate, key):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(work, certificate), os.path.join(work, key))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


# Bağlantıyı kabul edip TLS el sıkışmasına hiç cevap vermeyen port: curl'ün bağlantı evresi (el sıkışma dahil)
# bitmez, bağlantı zaman aşımı sınanır. Bağlantılar süreç ölene dek açık tutulur.
def serve_silent():
    listener = socket.create_server(("127.0.0.1", 0))
    held = []

    def accept_forever():
        while True:
            held.append(listener.accept()[0])

    threading.Thread(target=accept_forever, daemon=True).start()
    return listener.getsockname()[1]


# Test süreci ayrık (setsid) başlatır: ölünce sinyal gelmez. Pid'i yoklanır; yoksa sunucu hemen çıkar.
def exit_with_parent(parent_pid):
    def watch():
        while True:
            try:
                os.kill(parent_pid, 0)
            except ProcessLookupError:
                os._exit(0)
            time.sleep(PARENT_POLL_SECONDS)

    threading.Thread(target=watch, daemon=True).start()


def main():
    work = sys.argv[1]
    exit_with_parent(int(sys.argv[2]))
    make_certificates(work)
    trusted = serve(work, "server.pem", "server.key")
    untrusted = serve(work, "untrusted.pem", "untrusted.key")
    silent = serve_silent()
    wrong_host = serve(work, "wronghost.pem", "wronghost.key")
    with open(os.path.join(work, "ready.tmp"), "w") as ready:
        ready.write("%d %d %d %d\n" % (trusted, untrusted, silent, wrong_host))
    os.replace(os.path.join(work, "ready.tmp"), os.path.join(work, "ready.txt"))
    threading.Event().wait()


if __name__ == "__main__":
    try:
        main()
    except BaseException:
        with open(os.path.join(sys.argv[1], "server.log"), "w") as log:
            log.write(traceback.format_exc())
            if isinstance(sys.exc_info()[1], subprocess.CalledProcessError):
                log.write((sys.exc_info()[1].stderr or b"").decode(errors="replace"))
        raise
