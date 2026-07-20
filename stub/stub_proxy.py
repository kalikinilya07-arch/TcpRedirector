#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stub_proxy.py — приложение-заглушка HTTP(S)-прокси с имитацией Kerberos (SPNEGO/Negotiate).

Назначение
----------
Самостоятельная заглушка вышестоящего прокси-сервера для тестирования основного
приложения TcpRedirector (родительская папка). Заглушка:

  * поднимает HTTP(S)-прокси, принимает CONNECT (HTTPS-туннели) и обычные HTTP-методы;
  * проксирует трафик на целевые адреса, извлекаемые из входящих запросов;
  * имитирует Kerberos-аутентификацию (SPNEGO/Negotiate): принимает ЛЮБЫЕ
    предоставленные аутентификационные данные без реальной валидации и всегда
    пропускает запрос дальше;
  * логирует данные авторизации (заголовки, токены/креденшелы) и сведения о каждом
    проксируемом запросе (метод, целевой адрес, статус, временная метка).

Реализовано ТОЛЬКО на стандартной библиотеке Python 3 (без внешних зависимостей).
Код полностью независим от основного приложения и не изменяет его.

Разработано как тестовая заглушка. НЕ предназначено для продакшена.
"""

import argparse
import base64
import binascii
import json
import os
import select
import socket
import ssl
import sys
import threading
import time
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Значения по умолчанию
# ---------------------------------------------------------------------------

DEFAULTS = {
    "host": "127.0.0.1",
    "port": 8888,
    "auth_mode": "accept-any",      # accept-any | challenge | basic-log-only
    "tls": False,
    "cert": "",
    "key": "",
    "log_file": os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs", "stub_proxy.log"),
    "log_format": "text",           # text | json
    "log_max_mb": 10,
    "reveal_secrets": False,
    "connect_timeout": 10.0,
    "idle_timeout": 0.0,            # 0 = без таймаута простоя туннеля
    "buffer_size": 65536,
}

# Псевдо-challenge/финальный токен для имитации Negotiate (base64, не валидируется).
FAKE_NEGOTIATE_CHALLENGE = base64.b64encode(b"STUB-KERBEROS-CHALLENGE-TOKEN").decode("ascii")
FAKE_NEGOTIATE_FINAL = base64.b64encode(b"STUB-KERBEROS-FINAL-TOKEN").decode("ascii")

# Псевдо-NTLM Type-2 (challenge) токен. Это НЕ валидный NTLM-blob — заглушка
# лишь имитирует трёхногий цикл, а клиентский SSPI при accept-any/challenge
# всё равно не валидирует ответ сервера. Для реального NTLM-цикла нужен
# корректный Type-2, но цель заглушки — только прогнать обмен и залогировать.
FAKE_NTLM_CHALLENGE = base64.b64encode(b"NTLMSSP\x00\x02STUB-CHALLENGE").decode("ascii")

SERVER_NAME = "StubKerberosProxy/1.0"


def classify_negotiate_token(payload):
    """
    Определяет подтип токена схемы Negotiate/Kerberos по его содержимому.

    Возвращает человекочитаемую метку: 'NTLM Type1/2/3', 'Kerberos/SPNEGO'
    или 'unknown'. Используется только для диагностического логирования —
    валидацию заглушка не выполняет.
    """
    try:
        raw = base64.b64decode(payload, validate=False)
    except (binascii.Error, ValueError):
        return "unknown"
    if raw[:8] == b"NTLMSSP\x00":
        # 9-й байт — тип сообщения NTLM (1=Negotiate, 2=Challenge, 3=Authenticate)
        if len(raw) >= 12:
            msg_type = raw[8]
            names = {1: "NTLM Type1 (Negotiate)",
                     2: "NTLM Type2 (Challenge)",
                     3: "NTLM Type3 (Authenticate)"}
            return names.get(msg_type, "NTLM (unknown type)")
        return "NTLM (truncated)"
    # SPNEGO/Kerberos обёрнут в ASN.1: GSS-API начинается с 0x60 (APPLICATION 0)
    # или SPNEGO NegTokenInit с 0xA0/0x60; Kerberos AP-REQ — 0x6E.
    if raw[:1] in (b"\x60", b"\x6e", b"\xa1"):
        return "Kerberos/SPNEGO (GSS-API)"
    return "unknown"


# ---------------------------------------------------------------------------
# Логгер: пишет в консоль и в файл, поддерживает text/json и простую ротацию.
# ---------------------------------------------------------------------------

class Logger:
    def __init__(self, log_file, log_format="text", max_mb=10, reveal_secrets=False):
        self.log_format = log_format
        self.max_bytes = int(max_mb) * 1024 * 1024 if max_mb else 0
        self.reveal_secrets = reveal_secrets
        self._lock = threading.Lock()
        self._fh = None
        self.log_file = log_file
        if log_file:
            os.makedirs(os.path.dirname(os.path.abspath(log_file)), exist_ok=True)
            self._fh = open(log_file, "a", encoding="utf-8")

    @staticmethod
    def _now_iso():
        return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")

    def _rotate_if_needed(self):
        if not self._fh or not self.max_bytes:
            return
        try:
            if self._fh.tell() >= self.max_bytes:
                self._fh.close()
                bak = self.log_file + ".1"
                if os.path.exists(bak):
                    os.remove(bak)
                os.replace(self.log_file, bak)
                self._fh = open(self.log_file, "a", encoding="utf-8")
        except OSError:
            # Ротация — best-effort; не роняем сервер из-за файловых ошибок.
            pass

    def _emit(self, line):
        with self._lock:
            print(line, flush=True)
            if self._fh:
                self._fh.write(line + "\n")
                self._fh.flush()
                self._rotate_if_needed()

    def event(self, event, **fields):
        """Записать структурированное событие."""
        ts = self._now_iso()
        if self.log_format == "json":
            record = {"ts": ts, "event": event}
            record.update(fields)
            self._emit(json.dumps(record, ensure_ascii=False))
        else:
            parts = [f"{ts}", f"[{event}]"]
            for k, v in fields.items():
                parts.append(f"{k}={v}")
            self._emit(" ".join(parts))

    def info(self, msg):
        self.event("INFO", msg=msg)

    def error(self, msg):
        self.event("ERROR", msg=msg)

    def close(self):
        with self._lock:
            if self._fh:
                self._fh.close()
                self._fh = None


# ---------------------------------------------------------------------------
# Разбор аутентификации
# ---------------------------------------------------------------------------

def parse_authorization(headers, reveal_secrets):
    """
    Разбирает заголовок Proxy-Authorization (или Authorization).
    Возвращает словарь с полями для логирования. Валидацию НЕ выполняет.
    """
    raw = headers.get("proxy-authorization") or headers.get("authorization")
    if not raw:
        return {"auth_type": "none", "auth_raw": ""}

    result = {"auth_type": "unknown", "auth_raw": raw}
    parts = raw.split(None, 1)
    scheme = parts[0].lower() if parts else ""
    payload = parts[1].strip() if len(parts) > 1 else ""

    if scheme == "basic":
        result["auth_type"] = "Basic"
        result["auth_token"] = payload
        try:
            decoded = base64.b64decode(payload).decode("utf-8", errors="replace")
            if ":" in decoded:
                user, pwd = decoded.split(":", 1)
            else:
                user, pwd = decoded, ""
            result["basic_user"] = user
            if reveal_secrets:
                result["basic_password"] = pwd
            else:
                masked = "***" if pwd else "(empty)"
                result["basic_password"] = masked
        except (binascii.Error, ValueError):
            result["basic_decode_error"] = "true"
    elif scheme in ("negotiate", "kerberos"):
        result["auth_type"] = "Negotiate"
        result["negotiate_token_len"] = len(payload)
        # Диагностика: определяем подтип токена (NTLM Type1/2/3 или Kerberos).
        result["token_subtype"] = classify_negotiate_token(payload)
        if reveal_secrets:
            result["negotiate_token"] = payload
        else:
            # Показываем начало токена для диагностики, не раскрывая целиком.
            result["negotiate_token_preview"] = (payload[:24] + "...") if len(payload) > 24 else payload
    elif scheme == "ntlm":
        result["auth_type"] = "NTLM"
        result["ntlm_token_len"] = len(payload)
        result["token_subtype"] = classify_negotiate_token(payload)
        if reveal_secrets:
            result["ntlm_token"] = payload
        else:
            result["ntlm_token_preview"] = (payload[:24] + "...") if len(payload) > 24 else payload
    else:
        result["auth_type"] = scheme or "unknown"

    return result


# ---------------------------------------------------------------------------
# Разбор HTTP-запроса
# ---------------------------------------------------------------------------

def read_http_headers(sock, buffer_size):
    """
    Читает из сокета до конца заголовков (\r\n\r\n).
    Возвращает (request_line, headers_dict, leftover_body_bytes) или (None, None, None) при EOF.
    """
    data = b""
    while b"\r\n\r\n" not in data:
        try:
            chunk = sock.recv(buffer_size)
        except (socket.timeout, OSError):
            return None, None, None
        if not chunk:
            return None, None, None
        data += chunk
        if len(data) > 1024 * 1024:  # защита от переполнения
            break

    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    if not lines or not lines[0]:
        return None, None, None

    request_line = lines[0].decode("iso-8859-1", errors="replace")
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, _, v = line.partition(b":")
            headers[k.decode("iso-8859-1").strip().lower()] = v.decode("iso-8859-1").strip()
    return request_line, headers, rest


def parse_request_line(request_line):
    """Возвращает (method, target, version)."""
    pieces = request_line.split(" ")
    if len(pieces) < 3:
        return None, None, None
    return pieces[0].upper(), pieces[1], pieces[2]


def parse_connect_target(target):
    """CONNECT-цель имеет вид host:port."""
    if ":" in target:
        host, _, port = target.rpartition(":")
        try:
            return host, int(port)
        except ValueError:
            return host, 443
    return target, 443


def parse_absolute_uri(target, headers):
    """
    Для обычных методов target — абсолютный URI (http://host:port/path) или относительный.
    Возвращает (host, port, path).
    """
    host = None
    port = 80
    path = target
    if target.startswith("http://"):
        rest = target[len("http://"):]
        authority, _, path_part = rest.partition("/")
        path = "/" + path_part
        if ":" in authority:
            host, _, p = authority.rpartition(":")
            try:
                port = int(p)
            except ValueError:
                port = 80
        else:
            host = authority
    else:
        host_hdr = headers.get("host", "")
        if host_hdr:
            if ":" in host_hdr:
                host, _, p = host_hdr.rpartition(":")
                try:
                    port = int(p)
                except ValueError:
                    port = 80
            else:
                host = host_hdr
    return host, port, path


# ---------------------------------------------------------------------------
# Туннелирование
# ---------------------------------------------------------------------------

def relay_loop(sock_a, sock_b, buffer_size, idle_timeout):
    """
    Двунаправленный relay между двумя сокетами через select.
    Возвращает (bytes_a_to_b, bytes_b_to_a).
    """
    sock_a.setblocking(False)
    sock_b.setblocking(False)
    a_to_b = 0
    b_to_a = 0
    sockets = [sock_a, sock_b]
    timeout = idle_timeout if idle_timeout and idle_timeout > 0 else None
    while True:
        try:
            readable, _, exceptional = select.select(sockets, [], sockets, timeout)
        except (OSError, ValueError):
            break
        if not readable and not exceptional:
            # Таймаут простоя
            break
        if exceptional:
            break
        closed = False
        for s in readable:
            try:
                data = s.recv(buffer_size)
            except (BlockingIOError, InterruptedError):
                continue
            except OSError:
                closed = True
                break
            if not data:
                closed = True
                break
            try:
                if s is sock_a:
                    sock_b.sendall(data)
                    a_to_b += len(data)
                else:
                    sock_a.sendall(data)
                    b_to_a += len(data)
            except OSError:
                closed = True
                break
        if closed:
            break
    return a_to_b, b_to_a


# ---------------------------------------------------------------------------
# Обработчик клиента
# ---------------------------------------------------------------------------

class ClientHandler(threading.Thread):
    _counter_lock = threading.Lock()
    _counter = 0

    def __init__(self, client_sock, client_addr, config, logger):
        super().__init__(daemon=True)
        self.client_sock = client_sock
        self.client_addr = client_addr
        self.config = config
        self.logger = logger
        with ClientHandler._counter_lock:
            ClientHandler._counter += 1
            self.conn_id = ClientHandler._counter

    def _client_str(self):
        return f"{self.client_addr[0]}:{self.client_addr[1]}"

    def _send(self, data):
        try:
            self.client_sock.sendall(data)
            return True
        except OSError:
            return False

    def _send_407(self, keepalive=True, ntlm_challenge=False):
        """
        Отправляет 407 Proxy Authentication Required с challenge.

        Если ntlm_challenge=True, клиент прислал NTLM Type-1 — отвечаем
        конкретным `Proxy-Authenticate: Negotiate <NTLM-Type2>`, чтобы прогнать
        трёхногий NTLM-цикл (Type1 → Type2 → Type3). Иначе отдаём и «пустой»
        Negotiate (для Kerberos/SPNEGO), и псевдо-challenge — клиентский SSPI
        выберет подходящий вариант.
        """
        body = b"Proxy authentication required (stub Kerberos).\n"
        headers = [
            "HTTP/1.1 407 Proxy Authentication Required",
            f"Server: {SERVER_NAME}",
        ]
        if ntlm_challenge:
            # Конкретный Type-2 challenge для продолжения NTLM-обмена.
            headers.append("Proxy-Authenticate: Negotiate " + FAKE_NTLM_CHALLENGE)
        else:
            headers.append("Proxy-Authenticate: Negotiate " + FAKE_NEGOTIATE_CHALLENGE)
            # «Голый» Negotiate позволяет клиенту начать новый SPNEGO-цикл.
            headers.append("Proxy-Authenticate: Negotiate")
        headers += [
            "Content-Type: text/plain; charset=utf-8",
            f"Content-Length: {len(body)}",
            ("Proxy-Connection: keep-alive" if keepalive else "Proxy-Connection: close"),
            "",
            "",
        ]
        return self._send("\r\n".join(headers).encode("iso-8859-1") + body)

    def _send_502(self, reason):
        body = f"Stub proxy: upstream connection failed ({reason}).\n".encode("utf-8")
        headers = [
            "HTTP/1.1 502 Bad Gateway",
            f"Server: {SERVER_NAME}",
            "Content-Type: text/plain; charset=utf-8",
            f"Content-Length: {len(body)}",
            "Proxy-Connection: close",
            "",
            "",
        ]
        self._send("\r\n".join(headers).encode("iso-8859-1") + body)

    def run(self):
        try:
            self._handle_client()
        except Exception as exc:  # заглушка не должна падать из-за одного клиента
            self.logger.event("HANDLER_ERROR", conn=self.conn_id,
                              client=self._client_str(), error=repr(exc))
        finally:
            try:
                self.client_sock.close()
            except OSError:
                pass

    def _handle_client(self):
        cfg = self.config
        self.client_sock.settimeout(30.0)

        request_line, headers, leftover = read_http_headers(self.client_sock, cfg["buffer_size"])
        if request_line is None:
            self.logger.event("CONN_CLOSED_EARLY", conn=self.conn_id, client=self._client_str())
            return

        method, target, version = parse_request_line(request_line)
        if not method:
            self.logger.event("BAD_REQUEST", conn=self.conn_id, client=self._client_str(),
                              request_line=request_line)
            self._send(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return

        # --- Логируем авторизацию ---
        auth = parse_authorization(headers, cfg["reveal_secrets"])
        self.logger.event("AUTH", conn=self.conn_id, client=self._client_str(),
                          method=method, target=target, **auth)

        # --- Имитация Kerberos/NTLM challenge (если режим challenge) ---
        # accept-any / basic-log-only: пропускаем сразу (первый запрос → 200).
        # challenge: прогоняем ПОЛНЫЙ цикл обмена, даже если клиент уже прислал
        # токен в первом CONNECT (K3-фикс).
        #
        # Корневая проблема прежней логики: условие срабатывало только при
        # auth_type == "none", но основное приложение ВСЕГДА прикладывает
        # начальный токен (NTLM Type-1 или Kerberos), поэтому 407 не отправлялся
        # никогда и SSPI-цикл не прогонялся. Теперь challenge-режим:
        #   1) на ПЕРВОМ CONNECT всегда отвечает 407 (не важно, есть ли токен),
        #      выбирая NTLM-Type2-challenge, если пришёл NTLM Type-1;
        #   2) читает повторный запрос с новым токеном и, если это снова
        #      промежуточный этап NTLM (Type-1/Type-2), challenge'ит ещё раз;
        #   3) после нескольких итераций (или получив финальный токен) пропускает.
        if cfg["auth_mode"] == "challenge":
            max_legs = 3   # хватает на трёхногий NTLM (Type1→Type2→Type3)
            leg = 0
            while leg < max_legs:
                subtype = auth.get("token_subtype", "")
                # Финальный этап: NTLM Type-3 или Kerberos-токен — принимаем.
                if subtype.startswith("NTLM Type3") or subtype.startswith("Kerberos"):
                    break
                is_ntlm_type1 = subtype.startswith("NTLM Type1")
                self.logger.event("CHALLENGE_SENT", conn=self.conn_id,
                                  client=self._client_str(), status=407,
                                  target=target, leg=leg + 1,
                                  challenge=("NTLM-Type2" if is_ntlm_type1 else "Negotiate"))
                if not self._send_407(keepalive=True, ntlm_challenge=is_ntlm_type1):
                    return
                # Повторно читаем запрос с новым токеном на том же соединении.
                request_line, headers, leftover = read_http_headers(
                    self.client_sock, cfg["buffer_size"])
                if request_line is None:
                    self.logger.event("CONN_CLOSED_AFTER_407", conn=self.conn_id,
                                      client=self._client_str(), leg=leg + 1)
                    return
                method, target, version = parse_request_line(request_line)
                if not method:
                    self._send(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                    return
                auth = parse_authorization(headers, cfg["reveal_secrets"])
                self.logger.event("AUTH_RETRY", conn=self.conn_id,
                                  client=self._client_str(), method=method,
                                  target=target, leg=leg + 1, **auth)
                leg += 1
                # Если после challenge токен исчез (клиент сдался) — пропускаем.
                if auth["auth_type"] == "none":
                    break

        # На этом этапе принимаем ЛЮБЫЕ креденшелы (или их отсутствие) без валидации.
        if method == "CONNECT":
            self._handle_connect(target, headers, auth)
        else:
            self._handle_plain(method, target, version, headers, leftover, auth)

    def _handle_connect(self, target, headers, auth):
        cfg = self.config
        host, port = parse_connect_target(target)
        t_start = time.monotonic()

        upstream = self._open_upstream(host, port)
        if upstream is None:
            self.logger.event("REQUEST", conn=self.conn_id, client=self._client_str(),
                              method="CONNECT", target=f"{host}:{port}",
                              auth_type=auth["auth_type"], status=502,
                              latency_ms=round((time.monotonic() - t_start) * 1000, 1))
            self._send_502(f"connect {host}:{port}")
            return

        # ВАЖНО: основной проект строго проверяет "HTTP/1.x 200" в начале ответа
        # (см. TcpRelayServer.h). Формат ответа менять нельзя.
        resp = (
            f"HTTP/1.1 200 Connection established\r\n"
            f"Server: {SERVER_NAME}\r\n"
        )
        # При Negotiate отдаём финальный псевдо-токен для завершения SSPI-цикла.
        if auth["auth_type"] == "Negotiate":
            resp += f"Proxy-Authenticate: Negotiate {FAKE_NEGOTIATE_FINAL}\r\n"
        resp += "\r\n"

        if not self._send(resp.encode("iso-8859-1")):
            try:
                upstream.close()
            except OSError:
                pass
            return

        self.logger.event("REQUEST", conn=self.conn_id, client=self._client_str(),
                          method="CONNECT", target=f"{host}:{port}",
                          auth_type=auth["auth_type"], status=200,
                          latency_ms=round((time.monotonic() - t_start) * 1000, 1))

        # Двунаправленный туннель
        try:
            self.client_sock.settimeout(None)
            up, down = relay_loop(self.client_sock, upstream,
                                  cfg["buffer_size"], cfg["idle_timeout"])
            self.logger.event("TUNNEL_CLOSED", conn=self.conn_id, client=self._client_str(),
                              target=f"{host}:{port}",
                              bytes_client_to_target=up, bytes_target_to_client=down)
        finally:
            try:
                upstream.close()
            except OSError:
                pass

    def _handle_plain(self, method, target, version, headers, leftover, auth):
        cfg = self.config
        host, port, path = parse_absolute_uri(target, headers)
        t_start = time.monotonic()

        if not host:
            self.logger.event("REQUEST", conn=self.conn_id, client=self._client_str(),
                              method=method, target=target, auth_type=auth["auth_type"],
                              status=400)
            self._send(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return

        upstream = self._open_upstream(host, port)
        if upstream is None:
            self.logger.event("REQUEST", conn=self.conn_id, client=self._client_str(),
                              method=method, target=f"{host}:{port}{path}",
                              auth_type=auth["auth_type"], status=502,
                              latency_ms=round((time.monotonic() - t_start) * 1000, 1))
            self._send_502(f"connect {host}:{port}")
            return

        # Пересобираем запрос к origin-серверу: относительный путь + очищенные заголовки.
        out_headers = []
        out_headers.append(f"{method} {path} {version}")
        for k, v in headers.items():
            if k in ("proxy-authorization", "proxy-connection"):
                continue  # прокси-специфичные заголовки не передаём дальше
            if k == "connection":
                continue
            # Восстанавливаем «человеческий» регистр заголовка
            out_headers.append(f"{_titlecase_header(k)}: {v}")
        out_headers.append("Connection: close")
        out_headers.append("")
        out_headers.append("")
        request_bytes = "\r\n".join(out_headers).encode("iso-8859-1")

        try:
            upstream.sendall(request_bytes)
            if leftover:
                upstream.sendall(leftover)
        except OSError:
            self._send_502("send request")
            try:
                upstream.close()
            except OSError:
                pass
            return

        # Читаем ответ, определяем статус для логирования, ретранслируем клиенту.
        status_code = self._pump_response(upstream)
        self.logger.event("REQUEST", conn=self.conn_id, client=self._client_str(),
                          method=method, target=f"{host}:{port}{path}",
                          auth_type=auth["auth_type"], status=status_code,
                          latency_ms=round((time.monotonic() - t_start) * 1000, 1))
        try:
            upstream.close()
        except OSError:
            pass

    def _pump_response(self, upstream):
        """Ретранслирует ответ upstream -> client, возвращает распознанный HTTP-статус."""
        cfg = self.config
        status_code = 0
        first = True
        upstream.settimeout(cfg["connect_timeout"])
        try:
            while True:
                try:
                    data = upstream.recv(cfg["buffer_size"])
                except (socket.timeout, OSError):
                    break
                if not data:
                    break
                if first:
                    first = False
                    try:
                        line = data.split(b"\r\n", 1)[0].decode("iso-8859-1")
                        parts = line.split(" ")
                        if len(parts) >= 2 and parts[1].isdigit():
                            status_code = int(parts[1])
                    except (ValueError, IndexError):
                        pass
                if not self._send(data):
                    break
        finally:
            pass
        return status_code

    def _open_upstream(self, host, port):
        cfg = self.config
        try:
            upstream = socket.create_connection((host, port), timeout=cfg["connect_timeout"])
            upstream.settimeout(None)
            return upstream
        except OSError as exc:
            self.logger.event("UPSTREAM_ERROR", conn=self.conn_id, client=self._client_str(),
                              target=f"{host}:{port}", error=str(exc))
            return None


def _titlecase_header(name):
    return "-".join(part.capitalize() for part in name.split("-"))


# ---------------------------------------------------------------------------
# Сервер
# ---------------------------------------------------------------------------

class StubProxyServer:
    def __init__(self, config, logger):
        self.config = config
        self.logger = logger
        self._running = False
        self._listen_sock = None
        self._ssl_context = None
        if config["tls"]:
            self._ssl_context = self._build_ssl_context()

    def _build_ssl_context(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cert = self.config["cert"]
        key = self.config["key"]
        if cert and key and os.path.exists(cert) and os.path.exists(key):
            ctx.load_cert_chain(certfile=cert, keyfile=key)
            self.logger.info(f"TLS enabled with cert={cert}")
        else:
            gen_cert, gen_key = self._ensure_self_signed()
            ctx.load_cert_chain(certfile=gen_cert, keyfile=gen_key)
            self.logger.info(f"TLS enabled with self-signed cert={gen_cert}")
        return ctx

    def _ensure_self_signed(self):
        """
        Генерирует self-signed сертификат средствами stdlib, если возможно.
        Требует наличие модуля 'cryptography' ИЛИ утилиты openssl. Если ни того,
        ни другого нет — выбрасывает понятную ошибку.
        """
        base = os.path.dirname(os.path.abspath(self.config["log_file"]))
        cert_path = os.path.join(base, "stub_selfsigned.crt")
        key_path = os.path.join(base, "stub_selfsigned.key")
        if os.path.exists(cert_path) and os.path.exists(key_path):
            return cert_path, key_path

        os.makedirs(base, exist_ok=True)

        # Попытка 1: openssl из PATH (обычно доступен на dev-машинах)
        import shutil
        import subprocess
        openssl = shutil.which("openssl")
        if openssl:
            subprocess.run(
                [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-keyout", key_path, "-out", cert_path, "-days", "3650",
                 "-subj", "/CN=stub-proxy"],
                check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            return cert_path, key_path

        # Попытка 2: модуль cryptography, если установлен
        try:
            from cryptography import x509
            from cryptography.x509.oid import NameOID
            from cryptography.hazmat.primitives import hashes, serialization
            from cryptography.hazmat.primitives.asymmetric import rsa
            import datetime as _dt

            key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
            subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "stub-proxy")])
            cert = (
                x509.CertificateBuilder()
                .subject_name(subject)
                .issuer_name(issuer)
                .public_key(key.public_key())
                .serial_number(x509.random_serial_number())
                .not_valid_before(_dt.datetime.utcnow())
                .not_valid_after(_dt.datetime.utcnow() + _dt.timedelta(days=3650))
                .sign(key, hashes.SHA256())
            )
            with open(key_path, "wb") as f:
                f.write(key.private_bytes(
                    encoding=serialization.Encoding.PEM,
                    format=serialization.PrivateFormat.TraditionalOpenSSL,
                    encryption_algorithm=serialization.NoEncryption(),
                ))
            with open(cert_path, "wb") as f:
                f.write(cert.public_bytes(serialization.Encoding.PEM))
            return cert_path, key_path
        except ImportError:
            pass

        raise RuntimeError(
            "TLS запрошен, но self-signed сертификат создать нечем. "
            "Установите openssl в PATH или пакет 'cryptography', либо укажите --cert/--key."
        )

    def serve_forever(self):
        cfg = self.config
        self._listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen_sock.bind((cfg["host"], cfg["port"]))
        self._listen_sock.listen(128)
        self._running = True

        scheme = "https" if cfg["tls"] else "http"
        self.logger.event("SERVER_START", scheme=scheme, bind=f"{cfg['host']}:{cfg['port']}",
                          auth_mode=cfg["auth_mode"], log_format=cfg["log_format"],
                          log_file=cfg["log_file"])

        try:
            while self._running:
                try:
                    client_sock, client_addr = self._listen_sock.accept()
                except OSError:
                    break
                if self._ssl_context is not None:
                    try:
                        client_sock = self._ssl_context.wrap_socket(client_sock, server_side=True)
                    except (ssl.SSLError, OSError) as exc:
                        self.logger.event("TLS_HANDSHAKE_ERROR",
                                          client=f"{client_addr[0]}:{client_addr[1]}",
                                          error=str(exc))
                        try:
                            client_sock.close()
                        except OSError:
                            pass
                        continue
                handler = ClientHandler(client_sock, client_addr, cfg, self.logger)
                handler.start()
        finally:
            self.stop()

    def stop(self):
        self._running = False
        if self._listen_sock:
            try:
                self._listen_sock.close()
            except OSError:
                pass
            self._listen_sock = None


# ---------------------------------------------------------------------------
# Конфигурация: CLI + JSON (CLI имеет приоритет над JSON, JSON — над defaults)
# ---------------------------------------------------------------------------

def load_config():
    parser = argparse.ArgumentParser(
        description="Заглушка HTTP(S)-прокси с имитацией Kerberos (SPNEGO/Negotiate)."
    )
    parser.add_argument("--config", help="Путь к JSON-конфигу")
    parser.add_argument("--host", help="Адрес прослушивания (bind)")
    parser.add_argument("--port", type=int, help="Порт прокси")
    parser.add_argument("--auth-mode", choices=["accept-any", "challenge", "basic-log-only"],
                        dest="auth_mode", help="Режим имитации авторизации")
    parser.add_argument("--tls", action="store_true", default=None,
                        help="Включить TLS (HTTPS-прокси)")
    parser.add_argument("--cert", help="Путь к TLS-сертификату (PEM)")
    parser.add_argument("--key", help="Путь к приватному ключу (PEM)")
    parser.add_argument("--log-file", dest="log_file", help="Путь к файлу лога")
    parser.add_argument("--log-format", choices=["text", "json"], dest="log_format",
                        help="Формат лога")
    parser.add_argument("--log-max-mb", type=int, dest="log_max_mb",
                        help="Максимальный размер лога (МБ) до ротации")
    parser.add_argument("--reveal-secrets", action="store_true", default=None,
                        dest="reveal_secrets",
                        help="Логировать пароли/токены полностью (по умолчанию маскируются)")
    parser.add_argument("--connect-timeout", type=float, dest="connect_timeout",
                        help="Таймаут подключения к целевому серверу (сек)")
    parser.add_argument("--idle-timeout", type=float, dest="idle_timeout",
                        help="Таймаут простоя туннеля (сек; 0 = без таймаута)")

    args = parser.parse_args()

    config = dict(DEFAULTS)

    # Слой 1: JSON-файл
    if args.config:
        with open(args.config, "r", encoding="utf-8") as f:
            file_cfg = json.load(f)
        for key, value in file_cfg.items():
            norm = key.replace("-", "_")
            if norm in config:
                config[norm] = value

    # Слой 2: CLI (только явно заданные значения)
    for key in config:
        val = getattr(args, key, None)
        if val is not None:
            config[key] = val

    return config


def main():
    config = load_config()
    logger = Logger(
        log_file=config["log_file"],
        log_format=config["log_format"],
        max_mb=config["log_max_mb"],
        reveal_secrets=config["reveal_secrets"],
    )
    server = StubProxyServer(config, logger)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("Shutting down (Ctrl+C)")
    finally:
        server.stop()
        logger.event("SERVER_STOP")
        logger.close()


if __name__ == "__main__":
    main()
