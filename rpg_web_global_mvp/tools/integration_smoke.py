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
    def __init__(self, base_url: str, credential: str, parameter: str = "ticket"):
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
        path = f"{path_prefix}/ws?{urllib.parse.urlencode({parameter: credential})}"
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
    checks = 0

    def passed(name: str) -> None:
        nonlocal checks
        checks += 1
        print(f"PASS {name}")

    api = ApiClient(args.base_url)
    version_info = api.request("GET", "/api/version", expected=200).body
    require(version_info.get("version") == "64-cpp.25", "unexpected server version")
    require(version_info.get("pwa_update") is True, "PWA update flag missing")
    passed("PWA version endpoint")

    health = api.request("GET", "/api/health", expected=200)
    require(health.body.get("database") == "ok", "database health is not ok")
    require(health.headers.get("X-Frame-Options") == "DENY", "security headers missing")
    passed("health and security headers")

    status = api.request("GET", "/api/cpp/status", expected=200).body
    require(status.get("version") == "64-cpp.25", "unexpected C++ version")
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

    # 62-cpp.23: startup migration must seed the executable multi-hit / portrait-mark examples.
    skill_templates = api.request("GET", "/api/skill-templates", expected=200, token=admin_token).body.get("skills", [])
    draw_weapon = next((x for x in skill_templates if x.get("name") == "畫兵成真"), None)
    quick_mark = next((x for x in skill_templates if x.get("name") == "速寫標記"), None)
    require(draw_weapon is not None and draw_weapon.get("damage_formula") == "1+精神*2", "畫兵成真 template missing")
    require(int((draw_weapon.get("data") or {}).get("hit_count", 0)) == 3, "畫兵成真 is not configured as three hits")
    require(quick_mark is not None and len((quick_mark.get("data") or {}).get("event_triggers", [])) >= 5, "速寫標記 triggers missing")
    statuses = api.request("GET", "/api/admin/status-templates", expected=200, token=admin_token).body.get("statuses", [])
    attack_mark = next((x for x in statuses if x.get("name") == "畫像標記•攻"), None)
    heal_mark = next((x for x in statuses if x.get("name") == "畫像標記•療"), None)
    require(attack_mark is not None and int(attack_mark.get("max_stacks", 0)) == 20, "attack portrait mark missing")
    require(heal_mark is not None and int(heal_mark.get("max_stacks", 0)) == 20, "healing portrait mark missing")
    passed("event skill templates and portrait marks")

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
    player_id = me["user"]["id"]
    passed("registration, duplicate protection and current user")

    # 64-cpp.25: a fresh character starts at tier 0 and 0 -> 1 does not require a ritual.
    first_rank = api.request(
        "POST", f"/api/admin/players/{player_id}/character-rank/advance", expected=200, token=admin_token
    ).body
    first_rank_character = first_rank["character"]
    require(int(first_rank_character.get("tier", -1)) == 1, "0 -> 1 tier advance failed")
    require(int(first_rank_character.get("level", -1)) == 1, "0 -> 1 did not reset to tier level 1")
    require(int(first_rank_character.get("attribute_budget", -1)) == 50, "tier 1 budget must be 50")
    require(int(first_rank_character.get("attribute_points", -1)) == 50, "tier 1 should start with 50 unspent base points")
    passed("character tier 0 -> 1 and fixed tier-1 attribute budget")

    # 56-cpp.17: generic affix/reforge + special currency + customizable equipment slots.
    slots = api.request("GET", "/api/equipment-slots", expected=200, token=player_token).body
    require(len(slots.get("slots", [])) >= 8, "default equipment slots missing")
    currencies = api.request("GET", "/api/me/special-currencies", expected=200, token=player_token).body
    require(any(row.get("code") == "trait_core" for row in currencies.get("currencies", [])),
            "trait_core special currency missing")
    api.request(
        "PATCH", f"/api/admin/players/{player_id}/special-currencies", expected=200, token=admin_token,
        payload={"code": "trait_core", "delta": 7},
    )
    currencies = api.request("GET", "/api/me/special-currencies", expected=200, token=player_token).body
    trait_core = next((row for row in currencies.get("currencies", []) if row.get("code") == "trait_core"), {})
    require(int(trait_core.get("amount", 0)) >= 7, "trait_core adjustment failed")
    created_affix = api.request(
        "POST", "/api/admin/affixes", expected=200, token=admin_token,
        payload={"name": "smoke-affix-" + secrets.token_hex(3), "rank": "G", "category": "測試", "description": "smoke"},
    ).body
    affix_id = created_affix["affix"]["id"]
    custom_target = api.request(
        "POST", "/api/admin/affix-targets", expected=200, token=admin_token,
        payload={"owner_user_id": player_id, "target_type": "custom", "display_name": "smoke-target", "rank": "G", "data": {}},
    ).body
    require(custom_target.get("target", {}).get("target_type") == "custom", "generic affix target failed")
    passed("generic affix target, special currency and equipment slot APIs")

    # 59-cpp.20: mixed-rank pools + separate special affixes + equipment-to-puppet roll.
    smoke_suffix = secrets.token_hex(4)
    f_affix = api.request(
        "POST", "/api/admin/affixes", expected=200, token=admin_token,
        payload={"name": "smoke-f-" + smoke_suffix, "rank": "F", "affix_kind": "normal", "category": "測試"},
    ).body
    f_affix_id = f_affix["affix"]["id"]
    mixed_pool = api.request(
        "POST", "/api/admin/affix-pools", expected=200, token=admin_token,
        payload={
            "name": "smoke-mixed-" + smoke_suffix,
            "target_type": "custom",
            "rank": "G",
            "pool_kind": "normal",
            "mixed_ranks": True,
            "rank_weights": {"G": 70, "F": 30},
        },
    ).body["pool"]
    mixed_pool_id = mixed_pool["id"]
    api.request(
        "PATCH", f"/api/admin/affix-pools/{mixed_pool_id}/entries/{affix_id}",
        expected=200, token=admin_token, payload={"weight": 2, "active": True},
    )
    mixed_after = api.request(
        "PATCH", f"/api/admin/affix-pools/{mixed_pool_id}/entries/{f_affix_id}",
        expected=200, token=admin_token, payload={"weight": 1, "active": True},
    ).body["pool"]
    mixed_entries = mixed_after.get("entries", [])
    require(len(mixed_entries) == 2, "mixed-rank pool entries missing")
    require(all(float(e.get("probability", 0)) > 0 for e in mixed_entries),
            "mixed-rank final probability missing")
    require(all("rank_probability" in e and "within_rank_probability" in e for e in mixed_entries),
            "two-stage mixed-rank probability fields missing")

    special_affix = api.request(
        "POST", "/api/admin/affixes", expected=200, token=admin_token,
        payload={"name": "smoke-special-" + smoke_suffix, "rank": "G", "affix_kind": "special", "category": "特殊"},
    ).body
    special_affix_id = special_affix["affix"]["id"]
    special_pool = api.request(
        "POST", "/api/admin/affix-pools", expected=200, token=admin_token,
        payload={
            "name": "smoke-special-pool-" + smoke_suffix,
            "target_type": "puppet",
            "rank": "G",
            "pool_kind": "special",
            "mixed_ranks": False,
            "rank_weights": {},
        },
    ).body["pool"]
    special_pool_id = special_pool["id"]
    api.request(
        "PATCH", f"/api/admin/affix-pools/{special_pool_id}/entries/{special_affix_id}",
        expected=200, token=admin_token, payload={"weight": 1, "active": True},
    )

    source_name = "smoke-equipment-" + smoke_suffix
    source_target = api.request(
        "POST", "/api/admin/affix-targets", expected=200, token=admin_token,
        payload={
            "owner_user_id": player_id, "target_type": "equipment",
            "display_name": source_name, "rank": "G", "data": {},
        },
    ).body["target"]
    recipient_target = api.request(
        "POST", "/api/admin/affix-targets", expected=200, token=admin_token,
        payload={
            "owner_user_id": player_id, "target_type": "puppet",
            "display_name": "smoke-puppet-" + smoke_suffix, "rank": "G",
            "data": {"max_special_affixes": 2, "intrinsic_effects": [{"type": "smoke_intrinsic", "value": 1}]},
        },
    ).body["target"]
    source_target_id = source_target["id"]
    recipient_target_id = recipient_target["id"]
    require(len(recipient_target.get("affixes", [])) == 0, "recipient unexpectedly has normal affixes")
    require(len(recipient_target.get("special_affix_details", [])) == 0, "recipient unexpectedly has special affixes")
    require(len(recipient_target.get("intrinsic_effects", [])) == 1, "intrinsic effects were not stored separately")

    special_rule = api.request(
        "POST", "/api/admin/special-affix-rules", expected=200, token=admin_token,
        payload={
            "name": "smoke-rule-" + smoke_suffix,
            "source_target_type": "equipment",
            "source_name": source_name,
            "source_rank": "G",
            "recipient_target_type": "puppet",
            "pool_id": special_pool_id,
            "chance_percent": 100,
            "grant_count": 1,
            "max_special_affixes": 2,
            "once_per_pair": True,
            "active": True,
        },
    ).body["rule"]
    special_rule_id = special_rule["id"]

    targets_before = api.request("GET", "/api/me/affix-targets", expected=200, token=player_token).body
    source_before = next(t for t in targets_before.get("targets", []) if int(t.get("id", 0)) == int(source_target_id))
    require(any(int(r.get("id", 0)) == int(special_rule_id) for r in source_before.get("special_grant_rules", [])),
            "special affix rule not exposed to matching source")

    normal_before = len(recipient_target.get("affixes", []))
    rolled = api.request(
        "POST", f"/api/affix-targets/{source_target_id}/special-roll/{recipient_target_id}",
        expected=200, token=player_token, payload={"rule_id": special_rule_id},
    ).body
    require(rolled.get("success") is True, "100% special affix roll did not succeed")
    rolled_target = rolled.get("target", {})
    require(len(rolled_target.get("special_affix_details", [])) == 1, "special affix was not granted")
    require(len(rolled_target.get("affixes", [])) == normal_before,
            "special affix incorrectly consumed/changed a normal affix slot")
    api.request(
        "POST", f"/api/affix-targets/{source_target_id}/special-roll/{recipient_target_id}",
        expected=409, token=player_token, payload={"rule_id": special_rule_id},
    )
    passed("mixed-rank pool and separate equipment-to-puppet special affix roll")

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
            "great_way": 0,
        },
    ).body["character"]
    require(character["name"] == "煙嵐", "character name was not updated")
    require(character["gender"] == "未設定", "character gender was not updated")
    require(character["age"] == 25, "character age was not updated")
    require(character["height_cm"] == 178, "character height was not updated")
    require(character["birthday"] == "霜月十七", "character birthday was not updated")
    require(int(character.get("attribute_budget", -1)) == 50, "tier-1 attribute budget mismatch")
    require(int(character.get("attribute_spent", -1)) == 40, "five-base-stat spent total mismatch")
    require(int(character.get("attribute_points", -1)) == 10, "remaining base attribute points mismatch")
    require(int(character.get("max_hp", -1)) == 20, "derived max_hp mismatch")
    require(int(character.get("endurance", -1)) == 10, "derived endurance mismatch")
    require(int(character.get("max_sanity", character.get("sanity", -1))) == 12, "derived sanity mismatch")
    require(int(character.get("will", -1)) == 8, "derived will mismatch")
    passed("character fields, fixed-tier allocation and derived stats")

    # 62-cpp.23.1: auto combat-start field skill should be stored as generic skill data and really create a battlefield effect.
    field_suffix = secrets.token_hex(3)
    field_name = "smoke-墨界-" + field_suffix
    field_key = "smoke_ink_domain_" + field_suffix
    field_skill = api.request(
        "POST", "/api/admin/skill-templates", expected=200, token=admin_token,
        payload={
            "name": "smoke-field-skill-" + field_suffix,
            "category": "測試",
            "skill_type": "passive",
            "level": 1,
            "rank": "G",
            "description": "integration field skill",
            "data": {
                "activation_mode": "auto_combat_start",
                "field_definition": {
                    "key": field_key,
                    "name": field_name,
                    "priority": 77,
                    "stack_policy": "replace_same",
                    "duration_rounds": 0,
                    "effects": {},
                    "triggers": [],
                },
            },
        },
    ).body["skill"]
    field_skill_id = field_skill["id"]
    field_skill_data = field_skill.get("data") or {}
    require(field_skill_data.get("activation_mode") == "auto_combat_start", "field activation mode was not persisted")
    require((field_skill_data.get("field_definition") or {}).get("key") == field_key, "field definition was not persisted")
    owned_skills = list(character.get("skills") or [])
    owned_skills.append({"template_id": field_skill_id, "name": field_skill["name"]})
    api.request(
        "PATCH", f"/api/admin/characters/{character['id']}", expected=200, token=admin_token,
        payload={"skills": owned_skills},
    )
    passed("auto combat-start field skill persistence")

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

    # 64-cpp.25: within a big tier, DM advances level 1 -> 4; crossing the big tier is blocked until ritual success.
    for expected_level in (2, 3, 4):
        advanced = api.request(
            "POST", f"/api/admin/players/{player_id}/character-rank/advance", expected=200, token=admin_token
        ).body["character"]
        require(int(advanced.get("tier", -1)) == 1, "minor rank advance changed the big tier")
        require(int(advanced.get("level", -1)) == expected_level, f"minor rank did not reach level {expected_level}")
    api.request(
        "POST", f"/api/admin/players/{player_id}/character-rank/advance", expected=409, token=admin_token
    )
    passed("four minor levels and ritual gate at tier-1 level-4")

    # 47-cpp.8: magic/ritual learning and seven-element storage.
    current_character = api.request("GET", "/api/character", expected=200, token=player_token).body["character"]
    require(all(int(current_character.get("element_storage_caps", {}).get(e, -1)) == 0 for e in ["暗","光","金","木","水","火","土"]), "new character element caps must all start at zero")
    require(all(int(current_character.get("element_storage", {}).get(e, -1)) == 0 for e in ["暗","光","金","木","水","火","土"]), "new character stored elements must all start at zero")

    magic_name = "C++測試火魔法" + secrets.token_hex(2)
    ritual_name = "C++測試水儀式" + secrets.token_hex(2)
    magic = api.request("POST", "/api/admin/magic-studies", expected=200, token=admin_token, payload={
        "name": magic_name, "category": "整合測試", "rank": "G", "element": "火", "tree_x": 0, "tree_y": 0, "icon": "🔥", "point_cost": 2,
        "description": "整合測試用", "prerequisite_ids": [],
        "effects": {"element_storage_cap_bonus": {"火": 2}}, "active": True,
    }).body["study"]
    ritual = api.request("POST", "/api/admin/ritual-studies", expected=200, token=admin_token, payload={
        "name": ritual_name, "category": "整合測試", "ritual_type": "測試儀式", "rank": "G", "point_cost": 1,
        "description": "整合測試用", "layout_notes": "中央放置測試媒介", "prerequisite_ids": [],
        "layout_definition": {"slots": [{"id": "center", "label": "中央媒介", "expected": "測試媒介", "x": 50, "y": 50, "required": True}], "connections": []},
        "material_requirements": [], "element_requirements": {}, "ritual_steps": ["第一步"],
        "success_effects": [{"type": "event", "text": "儀式成功整合測試"}], "failure_effects": [],
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

    # 55-cpp.16: bulk import magic / recipe / shop through the shared C++ preview -> commit pipeline.
    bulk_magic_name = "批量魔法" + secrets.token_hex(2)
    bulk_recipe_name = "批量配方" + secrets.token_hex(2)
    bulk_shop_name = "批量商品" + secrets.token_hex(2)
    bulk_cases = [
        ("magics", [{"name": bulk_magic_name, "category": "批量測試", "rank": "G", "element": "光", "tree_x": 1, "tree_y": 0, "icon": "✦", "point_cost": 1, "description": "bulk", "prerequisite_ids": [], "effects": {"element_storage_cap_bonus": {"光": 1}}, "active": True}]),
        ("recipes", [{"name": bulk_recipe_name, "recipe_type": "item", "description": "bulk", "materials": [{"name": "鐵礦", "quantity": 2}], "output": {"name": "測試產物", "quantity": 1}, "config": {}}]),
        ("shop", [{"name": bulk_shop_name, "category": "批量測試分類", "item_type": "item", "description": "bulk", "price_weird": 12, "quantity": 1, "stock": 5, "purchase_limit": 2, "discount_percent": 10, "sell_percent": 50, "tags": ["批量"], "active": True}]),
    ]
    for kind, rows in bulk_cases:
        preview = api.request("POST", "/api/admin/bulk-import/preview", expected=200, token=admin_token, payload={"kind": kind, "rows": rows}).body
        require(preview.get("error_count") == 0 and preview.get("valid_count") == 1, f"bulk preview failed for {kind}")
        committed = api.request("POST", "/api/admin/bulk-import/commit", expected=200, token=admin_token, payload={"kind": kind, "rows": preview["rows"]}).body
        require(committed.get("failed") == 0 and committed.get("created") == 1, f"bulk commit failed for {kind}")
    bulk_magics = api.request("GET", "/api/admin/magic-studies", expected=200, token=admin_token).body["studies"]
    bulk_recipes = api.request("GET", "/api/admin/recipes", expected=200, token=admin_token).body["recipes"]
    bulk_shop = api.request("GET", "/api/admin/shop", expected=200, token=admin_token).body["items"]
    bulk_magic_id = next(int(x["id"]) for x in bulk_magics if x.get("name") == bulk_magic_name)
    bulk_recipe_id = next(int(x["id"]) for x in bulk_recipes if x.get("name") == bulk_recipe_name)
    bulk_shop_item = next(x for x in bulk_shop if x.get("name") == bulk_shop_name)
    require(bulk_shop_item.get("category_name") == "批量測試分類", "bulk shop category was not normalized")
    api.request("DELETE", f"/api/admin/magic-studies/{bulk_magic_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/recipes/{bulk_recipe_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/shop/{bulk_shop_item['id']}", expected=200, token=admin_token)
    passed("bulk import magic, recipes and shop items")

    # 55-cpp.16: player self-avatar plus DM editing another player's avatar.
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

    # 64-cpp.25: tier-1 level-4 must cross to tier 2 through a successful ascension ritual.
    ascension = api.request(
        "POST", "/api/admin/ritual-studies", expected=200, token=admin_token, payload={
            "name": "C++一階登階儀式" + secrets.token_hex(2),
            "category": "整合測試", "ritual_type": "登階儀式", "rank": "G",
            "point_cost": 0, "description": "64-cpp.25 ascension smoke",
            "ascension_from_tier": 1, "ascension_to_tier": 2,
            "requires_research": False, "layout_definition": {"slots": [], "connections": []},
            "material_requirements": [], "element_requirements": {}, "ritual_steps": [],
            "success_effects": [], "failure_effects": [], "cast_rounds": 0, "active": True,
        },
    ).body["study"]
    ascension_id = int(ascension["id"])
    api.request("POST", f"/api/ritual-studies/{ascension_id}/learn", expected=200, token=player_token)
    ascension_field = api.request(
        "POST", f"/api/rooms/{room_id}/ritual-field", expected=200, token=player_token, payload={"ritual_id": ascension_id}
    ).body["instance"]
    ascension_instance_id = int(ascension_field["id"])
    ascended = api.request(
        "POST", f"/api/rooms/{room_id}/ritual-field/{ascension_instance_id}/start", expected=200, token=player_token
    ).body
    require(ascended["instance"].get("status") == "success", "ascension ritual did not complete")
    ascended_character = ascended["character"]
    require(int(ascended_character.get("tier", -1)) == 2 and int(ascended_character.get("level", -1)) == 1,
            "ascension ritual did not move tier 1 level 4 to tier 2 level 1")
    require(int(ascended_character.get("attribute_budget", -1)) == 100, "tier 2 budget must be 100")
    require(int(ascended_character.get("attribute_points", -1)) == 60,
            "tier 2 should preserve 40 allocated points and expose 60 remaining")
    api.request("DELETE", f"/api/rooms/{room_id}/ritual-field/{ascension_instance_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/ritual-studies/{ascension_id}", expected=200, token=admin_token)
    passed("ascension ritual advances big tier and grants the new fixed attribute budget")

    # 63-cpp.24: ritual dual panels / instance placement lifecycle.
    field = api.request("POST", f"/api/rooms/{room_id}/ritual-field", expected=200, token=player_token, payload={"ritual_id": ritual_study_id}).body["instance"]
    ritual_instance_id = int(field["id"])
    require(field.get("status") == "placing", "ritual instance did not start in placing state")
    require(field.get("validation", {}).get("ready") is False, "ritual should not be ready before placement")
    placed = api.request("POST", f"/api/rooms/{room_id}/ritual-field/{ritual_instance_id}/place", expected=200, token=player_token, payload={"slot_id": "center", "label": "測試媒介"}).body["instance"]
    require(placed.get("validation", {}).get("ready") is True, "ritual layout did not become ready")
    started_ritual = api.request("POST", f"/api/rooms/{room_id}/ritual-field/{ritual_instance_id}/start", expected=200, token=player_token).body["instance"]
    require(started_ritual.get("status") == "channeling", "ritual with steps should enter channeling")
    completed_ritual = api.request("POST", f"/api/rooms/{room_id}/ritual-field/{ritual_instance_id}/advance", expected=200, token=player_token).body["instance"]
    require(completed_ritual.get("status") == "success", "ritual did not complete after final step")
    ritual_field = api.request("GET", f"/api/rooms/{room_id}/ritual-field", expected=200, token=player_token).body
    require(any(int(x.get("id", 0)) == ritual_instance_id for x in ritual_field.get("instances", [])), "ritual instance missing from ritual field")
    passed("ritual recipe layout, room placement, channeling and success lifecycle")

    # 63-cpp.24.1: ritual XP / level / points ledger and per-player unknown ritual research.
    research_ritual = api.request("POST", "/api/admin/ritual-studies", expected=200, token=admin_token, payload={
        "name": "C++未知儀式" + secrets.token_hex(2), "mystery_name": "？？？殘缺儀式", "category": "整合測試",
        "ritual_type": "研究測試", "rank": "G", "point_cost": 1, "description": "完整解析後才看得到",
        "requires_research": True, "hidden_until_discovered": False, "research_progress_per_point": 50,
        "research_xp_per_point": 0, "first_success_xp": 120, "repeat_success_xp": 0,
        "research_reveal_rules": {"name":10,"description":10,"materials":20,"elements":30,"layout":50,"steps":70,"success":90,"failure":100},
        "layout_definition": {"slots": [], "connections": []}, "material_requirements": [{"name":"秘密材料","quantity":1}],
        "element_requirements": {}, "ritual_steps": [], "success_effects": [], "failure_effects": [], "cast_rounds": 0, "active": True,
    }).body["study"]
    research_ritual_id = int(research_ritual["id"])
    ritual_library = api.request("GET", "/api/ritual-studies", expected=200, token=player_token).body
    unknown = next(x for x in ritual_library.get("studies", []) if int(x.get("id", 0)) == research_ritual_id)
    require(unknown.get("name") == "？？？殘缺儀式" and int(unknown.get("research_progress", -1)) == 0, "unknown ritual was not masked per player")
    api.request("POST", f"/api/ritual-studies/{research_ritual_id}/learn", expected=400, token=player_token)
    api.request("POST", f"/api/admin/players/{player_id_for_study}/study-points", expected=200, token=admin_token, payload={"kind":"ritual","delta":3})
    research_50 = api.request("POST", f"/api/ritual-studies/{research_ritual_id}/research", expected=200, token=player_token, payload={"points":1}).body
    require(int(research_50["research"]["progress"]) == 50, "ritual research did not advance to 50 percent")
    research_100 = api.request("POST", f"/api/ritual-studies/{research_ritual_id}/research", expected=200, token=player_token, payload={"points":1}).body
    require(int(research_100["research"]["progress"]) == 100 and research_100["study"].get("research_complete") is True, "ritual research did not complete")
    learned_research = api.request("POST", f"/api/ritual-studies/{research_ritual_id}/learn", expected=200, token=player_token).body["character"]
    require(int(learned_research["ritual_points"]) == 0, "ritual research + learning point ledger deducted the wrong amount")
    research_field = api.request("POST", f"/api/rooms/{room_id}/ritual-field", expected=200, token=player_token, payload={"ritual_id": research_ritual_id}).body["instance"]
    research_instance_id = int(research_field["id"])
    research_success = api.request("POST", f"/api/rooms/{room_id}/ritual-field/{research_instance_id}/start", expected=200, token=player_token).body
    require(research_success["instance"].get("status") == "success", "zero-step researched ritual did not complete immediately")
    rewarded_character = research_success["character"]
    require(int(rewarded_character.get("ritual_xp", 0)) >= 120 and int(rewarded_character.get("ritual_level", 0)) >= 2, "first ritual success XP did not level ritual study")
    require(int(rewarded_character.get("ritual_points", 0)) >= 1, "ritual level-up did not award a spendable ritual point")
    api.request("DELETE", f"/api/rooms/{room_id}/ritual-field/{research_instance_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/ritual-studies/{research_ritual_id}", expected=200, token=admin_token)
    passed("ritual XP, level-up points, unknown-recipe research and first-success anti-farm reward")

    # 60-cpp.21.3: wheel schema/API must work on the same fresh PostgreSQL used by room creation.
    wheel_name = "smoke-wheel-" + secrets.token_hex(3)
    wheel_created = api.request(
        "POST", "/api/admin/wheels", expected=200, token=admin_token,
        payload={
            "room_id": room_id,
            "name": wheel_name,
            "audience": "all",
            "active": True,
            "options": [
                {"label": "A", "weight": 1},
                {"label": "B", "weight": 1},
            ],
        },
    ).body
    wheel_id = int(wheel_created.get("entry", {}).get("id", 0))
    require(wheel_id > 0, "wheel create did not return id")
    player_wheels = api.request(
        "GET", f"/api/wheels?room_id={room_id}", expected=200, token=player_token
    ).body.get("wheels", [])
    require(any(int(w.get("id", 0)) == wheel_id for w in player_wheels), "room wheel not visible")
    spun = api.request(
        "POST", f"/api/wheels/{wheel_id}/spin", expected=200, token=player_token
    ).body
    require(spun.get("result", {}).get("label") in {"A", "B"}, "wheel spin result invalid")
    api.request("DELETE", f"/api/admin/wheels/{wheel_id}", expected=200, token=admin_token)
    passed("wheel create, list and spin")

    # 55-cpp.16: direct avatar upload persistence. Tiny valid 1x1 PNG.
    tiny_png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII="
    avatar_upload = request_json(base_url, "/api/me/avatar/upload", token=player_token, method="POST", body={"image_base64": tiny_png})
    require(str(avatar_upload.get("avatar_url", "")).startswith("/api/avatar-images/"), "player avatar upload did not return internal image URL")
    avatar_get = requests.get(base_url + avatar_upload["avatar_url"], timeout=20)
    require(avatar_get.status_code == 200 and avatar_get.headers.get("content-type", "").startswith("image/png"), "uploaded avatar image endpoint failed")

    # 55-cpp.16: structured rule engine create -> dry-run -> real PLAYER_ACTION -> state/log.
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
    require(action.get("ok") is True and action.get("action", {}).get("action") == "購買兔子血", "PLAYER_ACTION receipt missing")
    require(action.get("rule_engine_degraded") is False, "PLAYER_ACTION unexpectedly degraded rule engine on fresh schema")
    require(isinstance(action.get("snapshot"), dict) and int(action.get("snapshot", {}).get("room", {}).get("id", 0)) == int(room_id), "PLAYER_ACTION viewer snapshot missing")
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
    planar = api.request("GET", f"/api/rooms/{room_id}/map-nodes", expected=200, token=player_token).body
    require(len(planar.get("nodes", [])) >= 3, "planar map nodes missing")
    require(all("x_percent" in n and "y_percent" in n for n in planar.get("nodes", [])[:3]), "planar node coordinates missing")
    player_node = int(node_ids[1])
    # 55-cpp.16: real movement hooks + block_action enforcement.
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
    blocked_node = int(node_ids[2])
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
    # 61-cpp.22.1: deleting a referenced node must clear scene/player references and graph links.
    disposable = api.request(
        "POST", f"/api/admin/rooms/{room_id}/map-nodes", expected=200, token=admin_token,
        payload={"name": "刪除整合測試節點", "x_percent": 88, "y_percent": 88, "connections": [player_node]},
    ).body["node"]
    disposable_id = int(disposable["id"])
    api.request("POST", f"/api/admin/rooms/{room_id}/map-nodes/{disposable_id}/enter", expected=200, token=admin_token)
    api.request(
        "PATCH", f"/api/rooms/{room_id}/location", expected=200, token=player_token,
        payload={"node_id": disposable_id},
    )
    deleted = api.request(
        "DELETE", f"/api/admin/rooms/{room_id}/map-nodes/{disposable_id}", expected=200, token=admin_token
    ).body
    require(deleted.get("ok") is True and int(deleted.get("deleted_node_id", 0)) == disposable_id, "deleted_node_id missing")
    planar_after_delete = api.request("GET", f"/api/rooms/{room_id}/map-nodes", expected=200, token=player_token).body
    require(all(int(n.get("id", 0)) != disposable_id for n in planar_after_delete.get("nodes", [])), "deleted node still listed")
    require(planar_after_delete.get("current_map_node_id") is None, "player current node was not cleared when node deleted")
    neighbor = next(n for n in planar_after_delete.get("nodes", []) if int(n.get("id", 0)) == player_node)
    require(str(disposable_id) not in [str(x) for x in neighbor.get("connections", [])], "deleted node remained in neighbor connections")
    passed("planar map node deletion cleanup")
    # Restore a valid scene/player position so later offline-AI checks are independent of this deletion test.
    api.request("POST", f"/api/admin/rooms/{room_id}/map-nodes/{player_node}/enter", expected=200, token=admin_token)
    api.request("PATCH", f"/api/rooms/{room_id}/location", expected=200, token=player_token, payload={"node_id": player_node})

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

    # 60-cpp.21: monster five attributes + structured skills + optional Great Way + Boss phase + AI skill conditions.
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
            "attributes": {"agility": 120, "strength": 10, "constitution": 10, "spirit": 10, "luck": 17},
            # Intentionally use top-level combat convenience fields: 60-cpp.21 must persist them into config.
            "combat_attack_attribute": "agility",
            "combat_damage_formula": "1D1",
            "combat_damage_target": "hp",
            "combat_accuracy_bonus": 0,
            "combat_defense_attribute": "constitution",
            "combat_dodge_attribute": "agility",
            "boss_enabled": True,
            "skills": [{
                "id": "smoke_bite",
                "name": "測試撕咬",
                "type": "active",
                "attack_attribute": "agility",
                "damage_formula": "1D1",
                "damage_target": "hp",
                "accuracy_bonus": 0,
                "priority": 200,
                "action_cost": 1,
                "resource_cost": 0,
                "ai_condition": {"chance_percent": 100},
            }],
            "passive_skills": [{
                "id": "smoke_passive",
                "name": "測試被動",
                "type": "passive",
                "description": "不應被 AI 主動使用",
            }],
            "great_way": {
                "enabled": True,
                "name": "測試大道",
                "rank": "G",
                "resource_max": 9,
                "skills": [{
                    "id": "smoke_way",
                    "name": "測試大道技",
                    "type": "great_way",
                    "attack_attribute": "agility",
                    "damage_formula": "1D1",
                    "damage_target": "hp",
                    "accuracy_bonus": 0,
                    "priority": 300,
                    "action_cost": 1,
                    "resource_cost": 1,
                    "ai_condition": {"chance_percent": 100, "boss_phase": 1, "min_round": 1, "self_hp_lte": 100},
                }],
            },
            "boss_phases": [{
                "name": "測試二階",
                "trigger_type": "manual",
                "damage_multiplier": 1.2,
                "ai_behavior": "aggressive",
                "skills": ["測試大道技"],
                "heal_percent": 0,
            }],
        },
    ).body["monster"]
    monster_template_id = monster["id"]
    templates = api.request(
        "GET", "/api/admin/monster-templates", expected=200, token=admin_token
    ).body["monsters"]
    listed_monster = next((item for item in templates if item["id"] == monster_template_id), None)
    require(listed_monster is not None, "created monster template was not listed")
    require(int(listed_monster.get("attributes", {}).get("luck", -1)) == 17, "monster luck attribute was not persisted")
    require(any(x.get("name") == "測試撕咬" for x in listed_monster.get("skills", []) if isinstance(x, dict)),
            "monster active skill was not persisted")
    require(any(x.get("name") == "測試被動" for x in listed_monster.get("passive_skills", []) if isinstance(x, dict)),
            "monster passive skill was not persisted")
    require(listed_monster.get("great_way", {}).get("enabled") is True and listed_monster.get("great_way", {}).get("name") == "測試大道",
            "monster Great Way was not persisted")
    require(int(listed_monster.get("great_way", {}).get("resource_max", -1)) == 9, "monster Great Way resource was not persisted")
    require(len(listed_monster.get("boss_phases", [])) == 1, "formal monster Boss phases were not persisted")
    require(listed_monster.get("config", {}).get("combat_attack_attribute") == "agility",
            "top-level monster combat config was not merged into config")

    patched_monster = api.request(
        "PATCH", f"/api/admin/monster-templates/{monster_template_id}", expected=200, token=admin_token,
        payload={
            "attributes": {"luck": 18},
            "combat_accuracy_bonus": 7,
        },
    ).body["monster_template"]
    require(int(patched_monster.get("attributes", {}).get("luck", -1)) == 18 and int(patched_monster.get("attributes", {}).get("agility", -1)) == 120,
            "partial monster five-attribute PATCH overwrote another stat")
    require(int(patched_monster.get("config", {}).get("combat_accuracy_bonus", -1)) == 7,
            "monster top-level combat PATCH did not persist")

    added = api.request(
        "POST",
        f"/api/admin/rooms/{room_id}/monsters",
        expected=200,
        token=admin_token,
        payload={"template_id": monster_template_id},
    ).body
    monster_id = added["instance_id"]
    placed_monster = next((item for item in added["room"]["monsters"] if item["id"] == monster_id), None)
    require(placed_monster is not None, "room monster was not returned in snapshot")
    require(int(placed_monster.get("max_fifth", -1)) == 9 and int(placed_monster.get("current_fifth", -1)) == 9,
            "monster Great Way resource was not initialized in room instance")
    encountered = api.request(
        "POST",
        f"/api/admin/rooms/{room_id}/monsters/{monster_id}/encounter",
        expected=200,
        token=admin_token,
    ).body["room"]
    room_monster = next(item for item in encountered["monsters"] if item["id"] == monster_id)
    require(room_monster["status"] == "encountered", "monster did not enter encounter state")
    require(room_monster.get("great_way", {}).get("name") == "測試大道", "room snapshot lost monster Great Way")
    passed("monster five stats, skills, Great Way, Boss phases and room placement")

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
    active_fields = battle.get("field_effects", [])
    smoke_field = next((f for f in active_fields if f.get("definition_key") == field_key), None)
    require(smoke_field is not None and smoke_field.get("name") == field_name, "AUTO_COMBAT_START did not create field effect")
    require(int(smoke_field.get("priority", -1)) == 77 and smoke_field.get("stack_policy") == "replace_same",
            "field priority/stack policy mismatch")
    passed("auto combat-start field activation")

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
        if ai_steps == 0:
            require(ai_action.get("ai_result", {}).get("skill_name") == "測試大道技",
                    "monster AI did not select highest-priority eligible Great Way skill")
            require(combat.get("skill", {}).get("name") == "測試大道技",
                    "combat response did not expose selected monster skill")
        latest_room = ai_action["room"]
        after_monster = next(item for item in latest_room["monsters"] if item["id"] == monster_id)
        if ai_steps == 0:
            require(int(after_monster.get("current_fifth", -1)) == 8,
                    "monster Great Way resource cost was not consumed")
        if latest_room["room"]["current_actor_type"] == "monster":
            current_monster = next(item for item in latest_room["monsters"] if item["id"] == monster_id)
            expected = before_remaining if combat.get("attacker_critical_action_bonus") else before_remaining - 1
            require(current_monster["turn_actions_remaining"] == expected, "AI action counter mismatch")
        ai_steps += 1
    require(latest_room["room"]["current_actor_type"] == "player", "AI turn did not advance to player")
    require(latest_room["room"]["current_actor_ref_id"] == me["user"]["id"], "wrong player actor")
    passed("monster AI skill conditions, Great Way resource cost and action consumption")

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
    require(not ended.get("field_effects"), "combat field effects were not cleared at battle end")
    passed("battle initiative, action counts, field cleanup and combat turn progression")

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
    api.request(
        "DELETE", f"/api/admin/skill-templates/{field_skill_id}", expected=200, token=admin_token,
    )
    passed("monster and field-skill cleanup")

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
        ticket_response = api.request("POST", "/api/realtime-ticket", expected=200, token=player_token, payload={}).body
        ticket = str(ticket_response.get("ticket") or "")
        require(len(ticket) >= 32, "realtime ticket is missing or too short")
        websocket = WebSocketClient(args.base_url, ticket, "ticket")
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
        passed("native WebSocket ticket handshake, enter, chat and dice")

    # 55-cpp.16: 規則－戰鬥橋接狀態 API。
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
    api.request("DELETE", f"/api/admin/special-affix-rules/{special_rule_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/affix-pools/{special_pool_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/affix-pools/{mixed_pool_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/affixes/{special_affix_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/affixes/{f_affix_id}", expected=200, token=admin_token)
    api.request("DELETE", f"/api/admin/affixes/{affix_id}", expected=200, token=admin_token)
    api.request(
        "DELETE", f"/api/admin/users/{player_id}", expected=200, token=admin_token
    )
    api.request("GET", "/api/me", expected=401, token=player_token)
    api.request("DELETE", f"/api/admin/magic-studies/{magic_study_id}", expected=200, token=admin_token)
    try:
        api.request("DELETE", f"/api/rooms/{room_id}/ritual-field/{ritual_instance_id}", expected=200, token=admin_token)
    except Exception:
        pass
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
