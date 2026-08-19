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
    require(status.get("version") == "52-cpp.13", "unexpected C++ version")
    require(len(status.get("ported_http_routes", [])) == 244, "legacy route count is not 244")
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

    # 47-cpp.8: magic/ritual learning and seven-element storage.
    current_character = api.request("GET", "/api/character", expected=200, token=player_token).body["character"]
    require(all(int(current_character.get("element_storage_caps", {}).get(e, -1)) == 0 for e in ["暗","光","金","木","水","火","土"]), "new character element caps must all start at zero")
    require(all(int(current_character.get("element_storage", {}).get(e, -1)) == 0 for e in ["暗","光","金","木","水","火","土"]), "new character stored elements must all start at zero")

    magic_name = "C++測試火魔法" + secrets.token_hex(2)
    ritual_name = "C++測試水儀式" + secrets.token_hex(2)
    magic = api.request("POST", "/api/admin/magic-studies", expected=200, token=admin_token, payload={
        "name": magic_name, "category": "整合測試", "rank": "G", "point_cost": 2,
        "description": "整合測試用", "prerequisite_ids": [],
        "effects": {"element_storage_cap_bonus": {"火": 2}}, "active": True,
    }).body["study"]
    ritual = api.request("POST", "/api/admin/ritual-studies", expected=200, token=admin_token, payload={
        "name": ritual_name, "category": "整合測試", "rank": "G", "point_cost": 1,
        "description": "整合測試用", "prerequisite_ids": [],
        "effects": {"element_storage_cap_bonus": {"水": 1}}, "active": True,
    }).body["study"]
    magic_study_id = magic["id"]
    ritual_study_id = ritual["id"]
    player_id_for_study = me["user"]["id"]
    api.request("POST", f"/api/admin/players/{player_id_for_study}/study-points", expected=200, token=admin_token, payload={"kind":"magic","delta":2})
    api.request("POST", f"/api/admin/players/{player_id_for_study}/study-points", expected=200, token=admin_token, payload={"kind":"ritual","delta":1})
    learned_magic = api.request("POST", f"/api/magic-studies/{magic_study_id}/learn", expected=200, token=player_token).body["character"]
    require(int(learned_magic["magic_points"]) == 0, "magic study points were not deducted")
    require(int(learned_magic["element_storage_caps"]["火"]) == 2, "magic did not increase only fire storage cap")
    learned_ritual = api.request("POST", f"/api/ritual-studies/{ritual_study_id}/learn", expected=200, token=player_token).body["character"]
    require(int(learned_ritual["ritual_points"]) == 0, "ritual study points were not deducted")
    require(int(learned_ritual["element_storage_caps"]["水"]) == 1, "ritual did not increase water storage cap")
    adjusted = api.request("POST", f"/api/admin/players/{player_id_for_study}/element-storage", expected=200, token=admin_token, payload={"element":"暗","mode":"base_cap","delta":3}).body["character"]
    require(int(adjusted["element_storage_caps"]["暗"]) == 3, "DM permanent single-element cap adjustment failed")
    passed("magic/ritual points, learning and per-element storage caps")

    # 52-cpp.13: player self-avatar plus DM editing another player's avatar.
    avatar_url = "https://example.invalid/cpp-smoke-avatar.png"
    avatar_character = api.request(
        "PATCH", "/api/me/avatar", expected=200, token=player_token,
        payload={"avatar_url": avatar_url},
    ).body["character"]
    require(avatar_character.get("avatar_url") == avatar_url, "player avatar was not persisted")
    dm_avatar_url = "https://example.invalid/cpp-smoke-avatar-by-dm.png"
    dm_avatar_character = api.request(
        "PATCH", f"/api/admin/players/{me['user']['id']}/avatar", expected=200, token=admin_token,
        payload={"avatar_url": dm_avatar_url},
    ).body["character"]
    require(dm_avatar_character.get("avatar_url") == dm_avatar_url, "DM could not update another player's avatar")
    refreshed_avatar = api.request("GET", "/api/character", expected=200, token=player_token).body["character"]
    require(refreshed_avatar.get("avatar_url") == dm_avatar_url, "DM avatar update was not visible to the player")
    passed("player self-avatar and DM avatar override")
    rule_title = "C++世界規則" + secrets.token_hex(2)
    rule_entry = api.request(
        "POST", "/api/admin/rulebook", expected=200, token=admin_token,
        payload={"title": rule_title, "category": "整合測試", "content": "同場景交流測試規則", "sort_order": 1, "public": True, "active": True},
    ).body["entry"]
    rulebook_entry_id = rule_entry["id"]
    visible_rules = api.request("GET", "/api/rulebook", expected=200, token=player_token).body["entries"]
    require(any(entry.get("id") == rulebook_entry_id for entry in visible_rules), "public rulebook entry was not visible to player")
    passed("rulebook and editable player avatar")

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

    # 52-cpp.13: direct avatar upload persistence. Tiny valid 1x1 PNG.
    tiny_png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII="
    avatar_upload = request_json(base_url, "/api/me/avatar/upload", token=player_token, method="POST", body={"image_base64": tiny_png})
    require(str(avatar_upload.get("avatar_url", "")).startswith("/api/avatar-images/"), "player avatar upload did not return internal image URL")
    avatar_get = requests.get(base_url + avatar_upload["avatar_url"], timeout=20)
    require(avatar_get.status_code == 200 and avatar_get.headers.get("content-type", "").startswith("image/png"), "uploaded avatar image endpoint failed")

    # 52-cpp.13: structured rule engine create -> dry-run -> real PLAYER_ACTION -> state/log.
    engine_rule = api.request(
        "POST", "/api/admin/rule-engine/rules", expected=200, token=admin_token,
        payload={
            "room_id": room_id, "rulebook_entry_id": rulebook_entry_id,
            "code": "SMOKE-RULE", "name": "C++規則引擎測試", "trigger_event": "PLAYER_ACTION",
            "tags": ["整合測試", "規則怪談"], "priority": 999, "automatic": True, "notify_dm": False,
            "conditions": {"all": [{"path": "event.payload.action", "op": "contains", "value": "兔子血"}]},
            "effects": [{"type": "log_violation"}, {"type": "add_flag", "flag": "smoke_rule_violation"}],
            "active": True,
        },
    ).body["rule"]
    engine_rule_id = engine_rule["id"]
    dry = api.request(
        "POST", "/api/admin/rule-engine/test", expected=200, token=admin_token,
        payload={"room_id": room_id, "actor_user_id": me["user"]["id"], "event_type": "PLAYER_ACTION",
                 "context": {"action": "購買兔子血"}, "dry_run": True},
    ).body
    require(any(int(x.get("rule", {}).get("id", 0)) == int(engine_rule_id) for x in dry.get("matched", [])), "rule engine dry-run did not match")
    action = api.request(
        "POST", f"/api/rooms/{room_id}/actions", expected=200, token=player_token,
        payload={"action": "購買兔子血", "text": "購買兔子血"},
    ).body
    require(any(int(x.get("rule", {}).get("id", 0)) == int(engine_rule_id) for x in action.get("triggered", [])), "PLAYER_ACTION did not trigger structured rule")
    engine_state = api.request(
        "GET", f"/api/admin/rule-engine/state?room_id={room_id}&user_id={me['user']['id']}", expected=200, token=admin_token
    ).body
    require("smoke_rule_violation" in engine_state.get("state", {}).get("flags", []), "rule effect flag was not persisted")
    engine_logs = api.request("GET", "/api/admin/rule-engine/logs", expected=200, token=admin_token).body["logs"]
    require(any(int(x.get("rule_id", 0) or 0) == int(engine_rule_id) for x in engine_logs), "rule engine log was not persisted")
    passed("structured rule engine dry-run, PLAYER_ACTION, state and log")

    # 48-cpp.9: procedural map, player location, offline team auto-accept and AI follow.
    generated = api.request(
        "POST", f"/api/admin/rooms/{room_id}/map/generate", expected=200, token=admin_token,
        payload={"theme": "C++整合測試區", "count": 3, "replace": True},
    ).body
    require(int(generated.get("generated", 0)) == 3, "procedural map did not create three nodes")
    node_ids = generated.get("node_ids", [])
    require(len(node_ids) == 3, "procedural map node ids missing")
    player_node = int(node_ids[-1])
    # 52-cpp.13: real movement hooks + block_action enforcement.
    api.request(
        "POST", "/api/admin/rule-engine/rules", expected=200, token=admin_token,
        payload={
            "room_id": room_id, "code": "SMOKE-MOVE", "name": "移動事件測試",
            "trigger_event": "PLAYER_MOVE_ATTEMPT", "priority": 998, "automatic": True, "notify_dm": False,
            "conditions": {"all": [{"path": "event.payload.node_id", "op": "eq", "value": player_node}]},
            "effects": [{"type": "add_flag", "flag": "smoke_move_enter"}], "active": True,
        },
    )
    api.request(
        "PATCH", f"/api/rooms/{room_id}/location", expected=200, token=player_token,
        payload={"node_id": player_node},
    )
    engine_state = api.request(
        "GET", f"/api/admin/rule-engine/state?room_id={room_id}&user_id={me['user']['id']}", expected=200, token=admin_token
    ).body
    require("smoke_move_enter" in engine_state.get("state", {}).get("flags", []), "PLAYER_MOVE_ATTEMPT hook did not persist effect")
    blocked_node = int(node_ids[0])
    api.request(
        "POST", "/api/admin/rule-engine/rules", expected=200, token=admin_token,
        payload={
            "room_id": room_id, "code": "SMOKE-BLOCK", "name": "禁止地點測試",
            "trigger_event": "PLAYER_MOVE_ATTEMPT", "priority": 1000, "automatic": True, "notify_dm": False,
            "conditions": {"all": [{"path": "event.payload.node_id", "op": "eq", "value": blocked_node}]},
            "effects": [{"type": "log_violation"}, {"type": "block_action"}], "active": True,
        },
    )
    api.request(
        "PATCH", f"/api/rooms/{room_id}/location", expected=403, token=player_token,
        payload={"node_id": blocked_node},
    )
    comm = api.request("GET", f"/api/rooms/{room_id}/communication", expected=200, token=player_token).body
    require(int(comm.get("node_id")) == player_node, "blocked movement changed player location")
    require(comm.get("communication_device") is False, "fresh character unexpectedly has a communication device")
    passed("rule engine movement hook and block_action enforcement")
    require(int(comm.get("node_id")) == player_node, "player scene location was not used by communication rules")
    require(comm.get("communication_device") is False, "fresh character unexpectedly has a communication device")

    team = api.request(
        "POST", f"/api/rooms/{room_id}/teams", expected=200, token=admin_token,
        payload={"name": "離線AI測試隊"},
    ).body["team"]
    invite = api.request(
        "POST", f"/api/rooms/{room_id}/team-invites", expected=200, token=admin_token,
        payload={"team_id": team["id"], "target_user_id": me["user"]["id"]},
    ).body
    require(invite.get("auto_accepted") is True, "offline player did not auto-accept team invite")
    team_state = api.request("GET", f"/api/rooms/{room_id}/teams", expected=200, token=player_token).body
    require(team_state.get("my_team") and int(team_state["my_team"]["id"]) == int(team["id"]), "offline invite did not join player to team")
    ai_tick = api.request("POST", f"/api/admin/rooms/{room_id}/offline-ai/tick", expected=200, token=admin_token).body
    require(any(int(action.get("user_id", 0)) == int(me["user"]["id"]) and action.get("type") == "follow_team" for action in ai_tick.get("actions", [])), "offline AI did not follow team")
    passed("procedural map, communication location and offline AI team follow")

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
            ai_players = api.request("GET", f"/api/admin/rooms/{room_id}/offline-ai", expected=200, token=admin_token).body["players"]
            ai_player = next(item for item in ai_players if int(item["user_id"]) == int(me["user"]["id"]))
            require(ai_player.get("active") is False, "online player did not regain control from offline AI")
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

    # 52-cpp.13: 規則－戰鬥橋接狀態 API。
    bridge_state = api.request(
        "GET", f"/api/admin/rooms/{room_id}/rule-combat", expected=200, token=admin_token
    ).body
    require("victory_condition" in bridge_state, "rule-combat bridge missing victory_condition")
    bridge_state = api.request(
        "PATCH", f"/api/admin/rooms/{room_id}/rule-combat", expected=200, token=admin_token,
        payload={"victory_condition": {"type": "eliminate_enemies", "auto_end": False}},
    ).body
    require(bridge_state.get("victory_condition", {}).get("type") == "eliminate_enemies",
            "rule-combat bridge patch failed")
    passed("rule-combat bridge state and victory condition")

    left = api.request(
        "POST", f"/api/rooms/{room_id}/leave", expected=200, token=admin_token
    ).body
    require(left["room"]["status"] == "closed", "room leave/close endpoint failed")
    api.request(
        "DELETE", f"/api/admin/rooms/{room_id}", expected=200, token=admin_token
    )
    api.request("GET", f"/api/rooms/{room_id}", expected=403, token=player_token)
    passed("room close and permanent deletion")

    api.request("DELETE", f"/api/admin/rulebook/{rulebook_entry_id}", expected=200, token=admin_token)
    player_id = me["user"]["id"]
    api.request(
        "DELETE", f"/api/admin/users/{player_id}", expected=200, token=admin_token
    )
    api.request("GET", "/api/me", expected=401, token=player_token)
    api.request("DELETE", f"/api/admin/magic-studies/{magic_study_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/ritual-studies/{ritual_study_id}", expected=200, token=admin_token)
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
