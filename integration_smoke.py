#!/usr/bin/env python3
"""Standard-library smoke test for a running TRPG C++ preview server."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import secrets
import socket
import ssl
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from http.client import HTTPMessage
from typing import Any


class SmokeFailure(RuntimeError):
    pass


@dataclass
class Response:
    status: int
    body: Any
    headers: HTTPMessage


class ApiClient:
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip("/")

    def request(
        self,
        method: str,
        path: str,
        *,
        expected: int,
        token: str | None = None,
        payload: dict[str, Any] | None = None,
    ) -> Response:
        data = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token:
            headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(
            self.base_url + path, data=data, headers=headers, method=method
        )
        try:
            opened = urllib.request.urlopen(request, timeout=8)
        except urllib.error.HTTPError as error:
            opened = error
        raw = opened.read()
        try:
            body = json.loads(raw.decode("utf-8")) if raw else None
        except json.JSONDecodeError as error:
            raise SmokeFailure(f"{method} {path} did not return JSON: {error}") from error
        if opened.status != expected:
            raise SmokeFailure(
                f"{method} {path}: expected HTTP {expected}, got {opened.status}: {body}"
            )
        return Response(opened.status, body, opened.headers)


class WebSocketClient:
    def __init__(self, base_url: str, token: str):
        parsed = urllib.parse.urlsplit(base_url)
        secure = parsed.scheme == "https"
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise SmokeFailure("WebSocket smoke test needs an http:// or https:// base URL")
        port = parsed.port or (443 if secure else 80)
        raw_socket = socket.create_connection((parsed.hostname, port), timeout=8)
        if secure:
            raw_socket = ssl.create_default_context().wrap_socket(
                raw_socket, server_hostname=parsed.hostname
            )
        self.socket = raw_socket
        self.socket.settimeout(8)
        self.buffer = bytearray()

        path_prefix = parsed.path.rstrip("/")
        path = f"{path_prefix}/ws?{urllib.parse.urlencode({'token': token})}"
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        host = parsed.hostname
        default_port = 443 if secure else 80
        if port != default_port:
            host = f"{host}:{port}"
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.socket.sendall(request.encode("ascii"))
        header = self._read_until(b"\r\n\r\n")
        lines = header.decode("iso-8859-1").split("\r\n")
        if not lines or " 101 " not in lines[0]:
            raise SmokeFailure(f"WebSocket upgrade failed: {lines[0] if lines else header!r}")
        response_headers: dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                response_headers[name.strip().lower()] = value.strip()
        expected_accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode("ascii")
        if response_headers.get("sec-websocket-accept") != expected_accept:
            raise SmokeFailure("WebSocket server returned an invalid accept key")

    def _read_until(self, marker: bytes) -> bytes:
        while marker not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise SmokeFailure("WebSocket connection closed during handshake")
            self.buffer.extend(chunk)
        position = self.buffer.index(marker) + len(marker)
        result = bytes(self.buffer[:position])
        del self.buffer[:position]
        return result

    def _read_exact(self, length: int) -> bytes:
        while len(self.buffer) < length:
            chunk = self.socket.recv(max(4096, length - len(self.buffer)))
            if not chunk:
                raise SmokeFailure("WebSocket connection closed unexpectedly")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result

    def _send_frame(self, opcode: int, payload: bytes = b"") -> None:
        mask = os.urandom(4)
        length = len(payload)
        header = bytearray([0x80 | opcode])
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.socket.sendall(bytes(header) + masked)

    def send_event(self, event: str, data: dict[str, Any]) -> None:
        encoded = json.dumps(
            {"event": event, "data": data}, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        self._send_frame(0x1, encoded)

    def receive_json(self) -> dict[str, Any]:
        while True:
            first, second = self._read_exact(2)
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else b""
            payload = self._read_exact(length)
            if masked:
                payload = bytes(
                    value ^ mask[index % 4] for index, value in enumerate(payload)
                )
            if opcode == 0x8:
                raise SmokeFailure("WebSocket server closed the connection")
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode != 0x1:
                continue
            try:
                decoded = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise SmokeFailure(f"Invalid WebSocket JSON: {error}") from error
            if not isinstance(decoded, dict):
                raise SmokeFailure("WebSocket event was not an object")
            return decoded

    def wait_for(self, event: str, predicate=lambda _: True) -> dict[str, Any]:
        for _ in range(30):
            envelope = self.receive_json()
            if envelope.get("event") == "error:message":
                raise SmokeFailure(f"WebSocket error: {envelope.get('data')}")
            if envelope.get("event") == event and predicate(envelope.get("data")):
                return envelope
        raise SmokeFailure(f"WebSocket event {event!r} was not received")

    def close(self) -> None:
        try:
            self._send_frame(0x8)
        except OSError:
            pass
        self.socket.close()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SmokeFailure(message)


def event_exists(snapshot: Any, kind: str, key: str, value: Any) -> bool:
    if not isinstance(snapshot, dict):
        return False
    for event in snapshot.get("events", []):
        if event.get("type") == kind and event.get("payload", {}).get(key) == value:
            return True
    return False


def run(args: argparse.Namespace) -> None:
    api = ApiClient(args.base_url)
    checks = 0

    def passed(name: str) -> None:
        nonlocal checks
        checks += 1
        print(f"PASS {name}")

    health = api.request("GET", "/api/health", expected=200)
    require(health.body.get("database") == "ok", "database health is not ok")
    require(health.headers.get("X-Frame-Options") == "DENY", "security headers missing")
    passed("health and security headers")

    status = api.request("GET", "/api/cpp/status", expected=200).body
    require(status.get("version") == "44-cpp.5", "unexpected C++ version")
    require(len(status.get("ported_http_routes", [])) == 38, "route count is not 38")
    require(status.get("production_ready") is False, "preview marked production-ready")
    passed("migration status")

    api.request("GET", "/api/me", expected=401)
    passed("unauthenticated request rejected")

    admin = api.request(
        "POST",
        "/api/auth/login",
        expected=200,
        payload={"username": args.admin_username, "password": args.admin_password},
    ).body
    admin_token = admin["token"]
    require(admin["user"]["is_admin"] is True, "configured DM is not an administrator")
    passed("administrator login")

    username = "cppcheck" + secrets.token_hex(3)
    password = "smoke-test-password"
    registered = api.request(
        "POST",
        "/api/auth/register",
        expected=200,
        payload={"username": username, "password": password},
    ).body
    player_token = registered["token"]
    api.request(
        "POST",
        "/api/auth/register",
        expected=409,
        payload={"username": username, "password": password},
    )
    me = api.request("GET", "/api/me", expected=200, token=player_token).body
    require(me["user"]["username"] == username, "registered user mismatch")
    passed("registration, duplicate protection and current user")

    character = api.request(
        "PATCH",
        "/api/character",
        expected=200,
        token=player_token,
        payload={
            "name": "煙嵐",
            "gender": "未設定",
            "age": 25,
            "height_cm": 178,
            "birthday": "霜月十七",
            "self_intro": "C++ 整合測試角色",
            "agility": 10,
            "strength": 10,
            "constitution": 10,
            "spirit": 10,
            "attribute_points": 10,
            "great_way": 100000000000,
        },
    ).body["character"]
    require(character["name"] == "煙嵐", "character name was not updated")
    require(character["gender"] == "未設定", "character gender was not updated")
    require(character["age"] == 25, "character age was not updated")
    require(character["height_cm"] == 178, "character height was not updated")
    require(character["birthday"] == "霜月十七", "character birthday was not updated")
    require(character["great_way"] == 100000000000, "BIGINT maximum was truncated")
    require(character["fifth_current"] == 100000000000, "derived fifth resource mismatch")
    passed("character fields, allocation and BIGINT")

    api.request(
        "PATCH",
        "/api/character",
        expected=403,
        token=player_token,
        payload={"current_great_way": 1},
    )
    api.request(
        "PATCH",
        "/api/character",
        expected=403,
        token=player_token,
        payload={"faith": 1},
    )
    api.request(
        "PATCH",
        "/api/character",
        expected=403,
        token=player_token,
        payload={"name": "第二個名字"},
    )
    api.request(
        "PATCH",
        "/api/character",
        expected=400,
        token=player_token,
        payload={"strength": 11},
    )
    passed("character permission and point-total guards")

    created = api.request(
        "POST",
        "/api/rooms",
        expected=200,
        token=admin_token,
        payload={"name": "C++ 整合測試房間"},
    ).body
    room_id = created["room"]["id"]
    room_code = created["room"]["code"]
    require(len(room_code) == 6, "room code is not six characters")
    joined = api.request(
        "POST",
        "/api/rooms/join",
        expected=200,
        token=player_token,
        payload={"code": room_code.lower()},
    ).body
    require(len(joined["members"]) == 2, "room does not contain both members")
    listed = api.request("GET", "/api/rooms", expected=200, token=player_token).body
    require(any(room["id"] == room_id for room in listed["rooms"]), "joined room not listed")
    started = api.request(
        "POST", f"/api/rooms/{room_id}/start", expected=200, token=admin_token
    ).body
    require(started["room"]["status"] == "active", "room did not start")
    passed("room create, join, list and start")

    monster = api.request(
        "POST",
        "/api/admin/monster-templates",
        expected=200,
        token=admin_token,
        payload={
            "name": "C++ 整合測試怪物",
            "category": "測試",
            "rank": "G",
            "max_hp": 50,
            "attributes": {"agility": 120, "strength": 10, "constitution": 10, "spirit": 10},
            "config": {
                "combat_attack_attribute": "agility",
                "combat_damage_formula": "1D1",
                "combat_damage_target": "hp",
                "combat_accuracy_bonus": 0,
                "combat_defense_attribute": "constitution",
                "combat_dodge_attribute": "agility",
            },
        },
    ).body["monster"]
    monster_template_id = monster["id"]
    templates = api.request(
        "GET", "/api/admin/monster-templates", expected=200, token=admin_token
    ).body["monsters"]
    require(
        any(item["id"] == monster_template_id for item in templates),
        "created monster template was not listed",
    )
    added = api.request(
        "POST",
        f"/api/admin/rooms/{room_id}/monsters",
        expected=200,
        token=admin_token,
        payload={"template_id": monster_template_id},
    ).body
    monster_id = added["instance_id"]
    require(
        any(item["id"] == monster_id for item in added["room"]["monsters"]),
        "room monster was not returned in snapshot",
    )
    encountered = api.request(
        "POST",
        f"/api/admin/rooms/{room_id}/monsters/{monster_id}/encounter",
        expected=200,
        token=admin_token,
    ).body["room"]
    room_monster = next(item for item in encountered["monsters"] if item["id"] == monster_id)
    require(room_monster["status"] == "encountered", "monster did not enter encounter state")
    passed("monster template, room placement and encounter")

    battle = api.request(
        "POST", f"/api/rooms/{room_id}/battle/start", expected=200, token=admin_token
    ).body
    require(battle["room"]["battle_active"] is True, "battle did not start")
    require(battle["room"]["current_actor_type"] == "monster", "initiative order mismatch")
    require(battle["room"]["current_actor_ref_id"] == monster_id, "wrong first combat actor")
    player_member = next(item for item in battle["members"] if item["id"] == me["user"]["id"])
    require(player_member["max_hp"] == 20, "v39 HP formula constitution*2 mismatch")
    require(player_member["max_spirit"] == 10, "v39 spirit maximum mismatch")
    current_monster = next(item for item in battle["monsters"] if item["id"] == monster_id)
    require(current_monster["turn_actions_remaining"] == 3, "agility action count mismatch")

    ai_steps = 0
    latest_room = battle
    while latest_room["room"]["current_actor_type"] == "monster" and ai_steps < 12:
        before_monster = next(item for item in latest_room["monsters"] if item["id"] == monster_id)
        before_remaining = before_monster["turn_actions_remaining"]
        ai_action = api.request(
            "POST",
            f"/api/admin/rooms/{room_id}/combat/ai-act",
            expected=200,
            token=admin_token,
            payload={},
        ).body
        combat = ai_action["combat"]
        require(combat["attacker"]["type"] == "monster", "AI attacker type mismatch")
        require(combat["target"]["type"] == "player", "AI target type mismatch")
        require(1 <= combat["attack"]["roll"] <= 100, "AI D100 attack roll outside range")
        require(1 <= combat["reaction"]["roll"] <= 100, "player reaction roll outside range")
        latest_room = ai_action["room"]
        if latest_room["room"]["current_actor_type"] == "monster":
            current_monster = next(item for item in latest_room["monsters"] if item["id"] == monster_id)
            expected = before_remaining if combat.get("attacker_critical_action_bonus") else before_remaining - 1
            require(current_monster["turn_actions_remaining"] == expected, "AI action counter mismatch")
        ai_steps += 1
    require(latest_room["room"]["current_actor_type"] == "player", "AI turn did not advance to player")
    require(latest_room["room"]["current_actor_ref_id"] == me["user"]["id"], "wrong player actor")
    passed("AI D100 basic attack and action consumption")

    reaction = api.request(
        "PATCH",
        f"/api/rooms/{room_id}/combat/reaction",
        expected=200,
        token=player_token,
        payload={"mode": "defense"},
    ).body
    player_member = next(item for item in reaction["room"]["members"] if item["id"] == me["user"]["id"])
    require(player_member["combat_reaction"] == "defense", "combat reaction mode was not persisted")

    player_attack = api.request(
        "POST",
        f"/api/rooms/{room_id}/combat/basic-attack",
        expected=200,
        token=player_token,
        payload={"target_type": "monster", "target_id": monster_id},
    ).body
    combat = player_attack["combat"]
    require(combat["attacker"]["type"] == "player", "player basic attack attacker mismatch")
    require(combat["target"]["id"] == monster_id, "player basic attack target mismatch")
    require(combat["formula"]["formula"] == "1D力量", "player basic damage formula mismatch")
    require(1 <= combat["attack"]["roll"] <= 100, "player D100 attack roll outside range")
    after_player = player_attack["room"]
    if combat.get("attacker_critical_action_bonus"):
        require(after_player["room"]["current_actor_type"] == "player", "critical bonus action was not retained")
        after_player = api.request(
            "POST", f"/api/rooms/{room_id}/combat/pass", expected=200, token=player_token
        ).body["room"]
    require(after_player["room"]["round"] == 2, "combat round did not advance after player action")
    require(after_player["room"]["current_actor_type"] == "monster", "round did not wrap to monster")
    passed("player reaction setting, D100 basic attack and damage response")

    ended = api.request(
        "POST", f"/api/rooms/{room_id}/battle/end", expected=200, token=admin_token
    ).body
    require(ended["room"]["battle_active"] is False, "battle did not end")
    passed("battle initiative, action counts and combat turn progression")

    api.request(
        "DELETE",
        f"/api/admin/rooms/{room_id}/monsters/{monster_id}",
        expected=200,
        token=admin_token,
    )
    api.request(
        "DELETE",
        f"/api/admin/monster-templates/{monster_template_id}",
        expected=200,
        token=admin_token,
    )
    passed("monster cleanup")

    saved = api.request(
        "POST", f"/api/rooms/{room_id}/save", expected=200, token=admin_token
    ).body
    require(saved["room"]["status"] == "saved", "room did not enter saved state")
    reopened = api.request(
        "POST", f"/api/rooms/{room_id}/reopen", expected=200, token=admin_token
    ).body
    require(reopened["room"]["status"] == "lobby", "room did not reopen to lobby")
    api.request("POST", f"/api/rooms/{room_id}/start", expected=200, token=admin_token)
    closed = api.request(
        "POST", f"/api/admin/rooms/{room_id}/close", expected=200, token=admin_token
    ).body
    require(closed["room"]["room"]["status"] == "closed", "admin close did not close room")
    admin_reopened = api.request(
        "POST", f"/api/admin/rooms/{room_id}/reopen", expected=200, token=admin_token
    ).body
    require(
        admin_reopened["room"]["room"]["status"] == "lobby",
        "admin reopen did not restore lobby",
    )
    api.request("POST", f"/api/rooms/{room_id}/start", expected=200, token=admin_token)
    passed("room save, close and reopen lifecycle")

    users = api.request("GET", "/api/admin/users", expected=200, token=admin_token).body
    rooms = api.request("GET", "/api/admin/rooms", expected=200, token=admin_token).body
    characters = api.request(
        "GET", "/api/admin/characters", expected=200, token=admin_token
    ).body
    room_characters = api.request(
        "GET",
        f"/api/rooms/{room_id}/characters",
        expected=200,
        token=admin_token,
    ).body
    require(len(users["users"]) >= 2, "administrator user list is incomplete")
    require(len(rooms["rooms"]) >= 1, "administrator room list is empty")
    require(len(characters["characters"]) >= 2, "administrator character list is incomplete")
    require(len(room_characters["characters"]) == 2, "room character list is incomplete")
    passed("administrator read views")

    if not args.skip_websocket:
        websocket = WebSocketClient(args.base_url, player_token)
        try:
            websocket.wait_for("connect")
            websocket.send_event("room:enter", {"roomId": room_id})
            websocket.wait_for("room:snapshot")
            websocket.send_event(
                "chat:send", {"roomId": room_id, "text": "C++ WebSocket 測試"}
            )
            websocket.wait_for(
                "room:snapshot",
                lambda data: event_exists(data, "chat", "text", "C++ WebSocket 測試"),
            )
            websocket.send_event(
                "dice:roll", {"roomId": room_id, "notation": "2d6+3", "label": "測試"}
            )
            dice_snapshot = websocket.wait_for(
                "room:snapshot",
                lambda data: event_exists(data, "dice", "notation", "2d6+3"),
            )["data"]
            dice_event = next(
                event
                for event in dice_snapshot["events"]
                if event.get("type") == "dice"
                and event.get("payload", {}).get("notation") == "2d6+3"
            )
            total = dice_event["payload"]["total"]
            require(5 <= total <= 15, "dice total is outside the expected range")
        finally:
            websocket.close()
        passed("native WebSocket enter, chat and dice")

    left = api.request(
        "POST", f"/api/rooms/{room_id}/leave", expected=200, token=admin_token
    ).body
    require(left["room"]["status"] == "closed", "room leave/close endpoint failed")
    api.request(
        "DELETE", f"/api/admin/rooms/{room_id}", expected=200, token=admin_token
    )
    api.request("GET", f"/api/rooms/{room_id}", expected=403, token=player_token)
    passed("room close and permanent deletion")

    player_id = me["user"]["id"]
    api.request(
        "DELETE", f"/api/admin/users/{player_id}", expected=200, token=admin_token
    )
    api.request("GET", "/api/me", expected=401, token=player_token)
    passed("administrator player deletion and cascade cleanup")

    print(f"RESULT {checks} integration checks passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default=os.environ.get("SMOKE_BASE_URL", "http://127.0.0.1:10000"))
    parser.add_argument("--admin-username", default=os.environ.get("ADMIN_USERNAME"))
    parser.add_argument("--admin-password", default=os.environ.get("ADMIN_PASSWORD"))
    parser.add_argument("--skip-websocket", action="store_true")
    args = parser.parse_args()
    if not args.admin_username or not args.admin_password:
        parser.error("ADMIN_USERNAME and ADMIN_PASSWORD are required")
    try:
        run(args)
    except (SmokeFailure, OSError, KeyError, ValueError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
