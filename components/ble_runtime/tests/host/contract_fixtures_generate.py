#!/usr/bin/env python3
"""Generate C vectors from the Device Link contract fixtures.

Kind "core" (default) consumes
contracts/provisioning/fixtures/core/v2/{wire,framing,authorization,
error_responses}.json plus fixtures/domains/wifi/v1/operation_results.json
and emits contract_fixtures.inc for the host test
test_contract_fixtures.c. Kind "wifi" consumes
fixtures/domains/wifi/v1/{invalid,operation_results}.json and emits
contract_fixtures_wifi.inc for the device_link_service host test. The
generated file is a build artifact; the fixture checkout is the source
of truth.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

STATUS_VALUES = {
    "OK": 1,
    "MALFORMED_FRAME": 2,
    "UNSUPPORTED_VERSION": 3,
    "UNSUPPORTED_OPERATION": 4,
    "UNSUPPORTED_CAPABILITY": 5,
    "UNAUTHENTICATED": 6,
    "PERMISSION_DENIED": 7,
    "CONFIRMATION_REQUIRED": 8,
    "INVALID_ARGUMENT": 9,
    "BUSY": 10,
    "NOT_FOUND": 11,
    "RESOURCE_EXHAUSTED": 12,
    "CONFLICT": 13,
    "UNAVAILABLE": 14,
    "STORAGE": 15,
    "INTERNAL": 16,
}

STATE_VALUES = {
    "PENDING": 1,
    "RUNNING": 2,
    "SUCCEEDED": 3,
    "FAILED": 4,
    "CANCELED": 5,
}

INVALID_KINDS = {
    "non-minimal-auto-connect": 1,
    "open-with-password": 2,
    "personal-with-empty-password": 3,
    "unsupported-credential-security": 4,
}


def _c_bytes(hex_text: str) -> str:
    raw = bytes.fromhex(hex_text)
    if not raw:
        return "{0}"
    chunks = ", ".join(f"0x{b:02x}" for b in raw)
    return f"{{{chunks}}}"


def _emit_hex_array(out, name, hex_list, extra_fields=None) -> None:
    """Emit one row per vector: {hex bytes..., len, <extra>}."""
    for index, vector in enumerate(hex_list):
        hex_text = vector["hex"]
        raw = bytes.fromhex(hex_text)
        entries = ", ".join(f"0x{b:02x}" for b in raw)
        suffix = ""
        if extra_fields:
            suffix = ", " + ", ".join(
                str(vector[field]) for field in extra_fields)
        out.write(
            f"static const uint8_t {name}_{index}[] = {{{entries}}};\n")
    out.write(f"static const size_t {name}_count = {len(hex_list)};\n")


def generate(fixture_dir: Path, out_path: Path) -> None:
    wire = json.loads((fixture_dir / "wire.json").read_text(encoding="utf-8"))
    framing = json.loads(
        (fixture_dir / "framing.json").read_text(encoding="utf-8"))
    authorization = json.loads(
        (fixture_dir / "authorization.json").read_text(encoding="utf-8"))

    out = out_path.open("w", encoding="utf-8")
    out.write("/* Generated from contracts/provisioning fixtures; do not "
              "edit. */\n")
    out.write("#ifndef CONTRACT_FIXTURES_INC\n#define CONTRACT_FIXTURES_INC\n")

    def emit_group(out, name, vectors, fields):
        """Emit one byte array per vector plus pointer/value tables."""
        names = []
        for index, vector in enumerate(vectors):
            raw = bytes.fromhex(vector["hex"])
            entries = ", ".join(f"0x{b:02x}" for b in raw)
            out.write(f"static const uint8_t {name}_{index}[] = "
                      f"{{{entries}}};\n")
            names.append(f"{name}_{index}")
        if names:
            out.write(f"static const uint8_t *const {name}_hex[] = "
                      f"{{{', '.join(names)}}};\n")
            out.write(f"static const size_t {name}_len[] = "
                      + "{" +
                      ", ".join(str(len(bytes.fromhex(v["hex"])))
                                for v in vectors) + "};\n")
        for field in fields:
            values = []
            for vector in vectors:
                value = vector[field]
                if isinstance(value, bool):
                    values.append("true" if value else "false")
                elif isinstance(value, int):
                    values.append(str(value))
                elif field == "kind":
                    values.append("1" if value == "request" else "2")
                elif field == "disposition":
                    values.append(str({
                        "COMPLETE": 2, "NEW_PARTIAL": 0, "PARTIAL": 0,
                        "DUPLICATE": 1,
                    }[value]))
                else:
                    values.append(f'"{value}"')
            out.write(f"static const int {name}_{field}[] = "
                      f"{{{', '.join(values)}}};\n")
        out.write(f"static const size_t {name}_count = {len(vectors)};\n")

    # --- wire.json: link_state vectors (16 raw bytes + expected validity). --
    out.write("\n/* wire.json link_state vectors. */\n")
    emit_group(out, "s_link_state", wire["link_state"], ["valid"])

    # --- wire.json: application headers. ---
    out.write("\n/* wire.json application_headers. */\n")
    emit_group(out, "s_app_header", wire["application_headers"],
               ["kind", "recovery"])

    # --- wire.json: invalid application headers. ---
    out.write("\n/* wire.json invalid_application_headers. */\n")
    emit_group(out, "s_invalid_header", wire["invalid_application_headers"],
               [])

    # --- wire.json: response statuses. ---
    out.write("\n/* wire.json direction_cases. */\n")
    emit_group(out, "s_direction",
               [{"hex": vector["header_hex"],
                 "receiver": 1 if vector["receiver"] == "device" else 2,
                 "valid": vector["valid"]}
                for vector in wire["direction_cases"]],
               ["receiver", "valid"])

    out.write("\n/* wire.json responses (status bytes). */\n")
    emit_group(out, "s_response_status",
               [{"hex": vector["status_hex"]} for vector in wire["responses"]],
               [])

    # --- wire.json: link errors (status value -> status_hex). ---
    out.write("\n/* wire.json invalid_responses. */\n")
    emit_group(out, "s_invalid_resp",
               [{"hex": vector["status_hex"]}
                for vector in wire["invalid_responses"]], [])

    out.write("\n/* wire.json link_errors (value, status_hex). */\n")
    emit_group(out, "s_link_error",
               [{"hex": vector["status_hex"], "value": vector["value"]}
                for vector in wire["link_errors"]], ["value"])

    # --- wire.json: advertising service data. ---
    out.write("\n/* wire.json advertising (service data, 5 bytes). */\n")
    adv_public = bytes.fromhex(wire["advertising"]["public"])
    adv_bindable = bytes.fromhex(wire["advertising"]["bindable"])
    out.write("static const uint8_t s_adv_public[] = {" +
              ", ".join(f"0x{b:02x}" for b in adv_public) + "};\n")
    out.write("static const uint8_t s_adv_bindable[] = {" +
              ", ".join(f"0x{b:02x}" for b in adv_bindable) + "};\n")

    # --- wire.json: channel_methods matrix. ---
    out.write("\n/* wire.json channel_methods. */\n")
    out.write("static const uint8_t s_channel_domain[] = {" +
              ", ".join(str(v["domain_id"])
                        for v in wire["channel_methods"]) + "};\n")
    out.write("static const uint8_t s_channel_method[] = {" +
              ", ".join(str(v["method_id"])
                        for v in wire["channel_methods"]) + "};\n")
    out.write("static const uint8_t s_channel_kind[] = {" +
              ", ".join("1" if v["channel"] == "session" else "2"
                        for v in wire["channel_methods"]) + "};\n")
    out.write(f"static const size_t s_channel_method_count = "
              f"{len(wire['channel_methods'])};\n")

    # --- framing.json: valid standalone fragments. ---
    out.write("\n/* framing.json valid fragments. */\n")
    emit_group(out, "s_frag_valid", framing["valid"], ["disposition"])

    # --- framing.json: invalid standalone fragments. ---
    out.write("\n/* framing.json invalid fragments. */\n")
    emit_group(out, "s_frag_invalid", framing["invalid"], [])

    # --- framing.json: valid sequences. ---
    out.write("\n/* framing.json valid sequences. */\n")
    sequence_vectors = framing["sequences"]["valid"]
    for seq_index, vector in enumerate(sequence_vectors):
        fragments = vector["fragments"]
        names = []
        lens = []
        for frag_index, fragment in enumerate(fragments):
            raw = bytes.fromhex(fragment)
            entries = ", ".join(f"0x{b:02x}" for b in raw)
            name = f"s_seq_{seq_index}_frag_{frag_index}"
            out.write(f"static const uint8_t {name}[] = {{{entries}}};\n")
            names.append(name)
            lens.append(str(len(raw)))
        out.write(f"static const uint8_t *const s_seq_{seq_index}_frags[] = "
                  f"{{{', '.join(names)}}};\n")
        out.write(f"static const size_t s_seq_{seq_index}_frag_lens[] = "
                  f"{{{', '.join(lens)}}};\n")
        out.write(f"static const size_t s_seq_{seq_index}_frag_count = "
                  f"{len(fragments)};\n")
        out.write(f"static const int s_seq_{seq_index}_disposition = "
                  + str({
                      "COMPLETE": 2,
                      "PARTIAL": 0,
                      "DUPLICATE": 1,
                  }[vector["disposition"]]) + ";\n")
    out.write(f"static const size_t s_seq_count = {len(sequence_vectors)};\n")
    out.write("static const uint8_t *const *const s_seq_frags_all[] = {" +
              ", ".join(f"s_seq_{i}_frags"
                        for i in range(len(sequence_vectors))) + "};\n")
    out.write("static const size_t *const s_seq_lens_all[] = {" +
              ", ".join(f"s_seq_{i}_frag_lens"
                        for i in range(len(sequence_vectors))) + "};\n")
    out.write("static const size_t s_seq_frag_counts_all[] = {" +
              ", ".join(str(len(v["fragments"]))
                        for v in sequence_vectors) + "};\n")
    out.write("static const int s_seq_disposition_all[] = {" +
              ", ".join(str({
                  "COMPLETE": 2, "PARTIAL": 0, "DUPLICATE": 1,
              }[v["disposition"]]) for v in sequence_vectors) + "};\n")

    # --- authorization.json: request/response bodies for schema checks. ---
    out.write("\n/* authorization.json bodies (schema-checked in C). */\n")
    body_keys = [
        ("prepare", 3, False),
        ("prepare_response", 3, True),
        ("commit_probe", 4, False),
        ("confirmation_required", 4, True),
        ("commit_success", 4, True),
    ]
    for name, method_id, is_response in body_keys:
        vector = authorization[name]
        raw = bytes.fromhex(vector["body_hex"])
        entries = ", ".join(f"0x{b:02x}" for b in raw)
        out.write(f"static const uint8_t s_auth_{name}[] = {{{entries}}};\n")
        out.write(f"static const size_t s_auth_{name}_len = {len(raw)};\n")
        out.write(f"static const uint8_t s_auth_{name}_method = "
                  f"{method_id};\n")
        out.write(f"static const bool s_auth_{name}_is_response = "
                  f"{'true' if is_response else 'false'};\n")

    # --- error_responses.json: non-OK responses with empty bodies. ---
    out.write("\n/* error_responses.json cases. */\n")
    error_responses = json.loads(
        (fixture_dir / "error_responses.json").read_text(encoding="utf-8"))
    for index, case in enumerate(error_responses["cases"]):
        out.write(f"static const uint8_t s_err_hdr_{index}[] = "
                  f"{_c_bytes(case['header_hex'])};\n")
        out.write(f"static const size_t s_err_hdr_{index}_len = "
                  f"{len(bytes.fromhex(case['header_hex']))};\n")
        out.write(f"static const uint8_t s_err_status_{index}[] = "
                  f"{_c_bytes(case['status_hex'])};\n")
        out.write(f"static const size_t s_err_status_{index}_len = "
                  f"{len(bytes.fromhex(case['status_hex']))};\n")
        out.write(f"static const int s_err_status_{index}_value = "
                  f"{STATUS_VALUES[case['status']]};\n")
        out.write(f"static const uint8_t s_err_domain_{index} = "
                  f"{case['domain_id']};\n")
        out.write(f"static const uint8_t s_err_major_{index} = "
                  f"{case['domain_major']};\n")
        out.write(f"static const uint8_t s_err_method_{index} = "
                  f"{case['method_id']};\n")
        out.write(f"static const size_t s_err_body_{index}_len = "
                  f"{len(bytes.fromhex(case['body_hex']))};\n")
    out.write(f"static const size_t s_err_count = "
              f"{len(error_responses['cases'])};\n")
    out.write("static const uint8_t *const s_err_hdr[] = {" +
              ", ".join(f"s_err_hdr_{i}"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const size_t s_err_hdr_len[] = {" +
              ", ".join(f"s_err_hdr_{i}_len"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const uint8_t *const s_err_status[] = {" +
              ", ".join(f"s_err_status_{i}"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const size_t s_err_status_len[] = {" +
              ", ".join(f"s_err_status_{i}_len"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const int s_err_status_value[] = {" +
              ", ".join(f"s_err_status_{i}_value"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const uint8_t s_err_domain[] = {" +
              ", ".join(f"s_err_domain_{i}"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const uint8_t s_err_major[] = {" +
              ", ".join(f"s_err_major_{i}"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const uint8_t s_err_method[] = {" +
              ", ".join(f"s_err_method_{i}"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")
    out.write("static const size_t s_err_body_len[] = {" +
              ", ".join(f"s_err_body_{i}_len"
                        for i in range(len(error_responses["cases"]))) +
              "};\n")

    # --- operation_results.json: OperationStatus body goldens. ---
    out.write("\n/* wifi.v1 operation_results.json bodies. */\n")
    op_results = json.loads(
        (_wifi_fixture_dir(fixture_dir) / "operation_results.json")
        .read_text(encoding="utf-8"))
    op_cases = op_results["cases"] + op_results["error_cases"]
    for index, case in enumerate(op_cases):
        out.write(f"static const uint8_t s_op_body_{index}[] = "
                  f"{_c_bytes(case['body_hex'])};\n")
        out.write(f"static const size_t s_op_body_{index}_len = "
                  f"{len(bytes.fromhex(case['body_hex']))};\n")
        out.write(f"static const uint8_t s_op_result_{index}[] = "
                  f"{_c_bytes(case['result_payload_hex'])};\n")
        out.write(f"static const size_t s_op_result_{index}_len = "
                  f"{len(bytes.fromhex(case['result_payload_hex']))};\n")
        out.write(f"static const uint64_t s_op_id_{index} = "
                  f"{case['operation_id']}ULL;\n")
        out.write(f"static const uint8_t s_op_method_{index} = "
                  f"{case['method_id']};\n")
        out.write(f"static const int s_op_state_{index} = "
                  f"{STATE_VALUES[case['state']]};\n")
        out.write(f"static const int s_op_error_{index} = "
                  f"{STATUS_VALUES[case['error']]};\n")
    out.write(f"static const size_t s_op_count = {len(op_cases)};\n")
    out.write("static const uint8_t *const s_op_body[] = {" +
              ", ".join(f"s_op_body_{i}" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const size_t s_op_body_len[] = {" +
              ", ".join(f"s_op_body_{i}_len" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const uint8_t *const s_op_result[] = {" +
              ", ".join(f"s_op_result_{i}" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const size_t s_op_result_len[] = {" +
              ", ".join(f"s_op_result_{i}_len"
                        for i in range(len(op_cases))) + "};\n")
    out.write("static const uint64_t s_op_id[] = {" +
              ", ".join(f"s_op_id_{i}" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const uint8_t s_op_method[] = {" +
              ", ".join(f"s_op_method_{i}" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const int s_op_state[] = {" +
              ", ".join(f"s_op_state_{i}" for i in range(len(op_cases))) +
              "};\n")
    out.write("static const int s_op_error[] = {" +
              ", ".join(f"s_op_error_{i}" for i in range(len(op_cases))) +
              "};\n")

    # --- golden.json: the device-applicable canonical messages. ---
    out.write("\n/* core/v2 golden.json (device-applicable goldens). */\n")
    goldens = json.loads(
        (fixture_dir / "golden.json").read_text(encoding="utf-8"))
    query = next(g for g in goldens if g["id"] == "operation-query")
    pending = next(g for g in goldens if g["id"] == "pending-operation")
    out.write(f"static const uint8_t s_golden_query[] = "
              f"{_c_bytes(query['canonical_hex'])};\n")
    out.write(f"static const size_t s_golden_query_len = "
              f"{len(bytes.fromhex(query['canonical_hex']))};\n")
    out.write(f"static const uint8_t s_golden_pending[] = "
              f"{_c_bytes(pending['canonical_hex'])};\n")
    out.write(f"static const size_t s_golden_pending_len = "
              f"{len(bytes.fromhex(pending['canonical_hex']))};\n")
    out.write(f"static const uint64_t s_golden_pending_id = "
              f"{pending['value']['operation_id']}ULL;\n")
    out.write(f"static const uint8_t s_golden_pending_domain = "
              f"{pending['value']['domain_id']};\n")
    out.write(f"static const uint8_t s_golden_pending_method = "
              f"{pending['value']['method_id']};\n")
    out.write(f"static const int s_golden_pending_state = "
              f"{STATE_VALUES[pending['value']['state']]};\n")
    out.write(f"static const int s_golden_pending_error = "
              f"{STATUS_VALUES[pending['value']['error']]};\n")

    out.write("\n#endif /* CONTRACT_FIXTURES_INC */\n")
    out.close()


def _wifi_fixture_dir(fixture_dir: Path) -> Path:
    """Derive the Wi-Fi v1 fixture dir from the core/v2 dir."""
    return fixture_dir.parent.parent / "domains" / "wifi" / "v1"


def generate_wifi(fixture_dir: Path, out_path: Path) -> None:
    """Emit wifi.v1 invalid.json and operation_results.json vectors."""
    wifi_dir = _wifi_fixture_dir(fixture_dir)
    invalid = json.loads((wifi_dir / "invalid.json").read_text(
        encoding="utf-8"))
    op_results = json.loads((wifi_dir / "operation_results.json").read_text(
        encoding="utf-8"))

    out = out_path.open("w", encoding="utf-8")
    out.write("/* Generated from contracts/provisioning fixtures; do not "
              "edit. */\n")
    out.write("#ifndef CONTRACT_FIXTURES_WIFI_INC\n"
              "#define CONTRACT_FIXTURES_WIFI_INC\n")

    out.write("\n/* wifi.v1 invalid.json wire vectors. */\n")
    for index, case in enumerate(invalid):
        raw = bytes.fromhex(case["wire_hex"])
        out.write(f"static const uint8_t s_wifi_invalid_{index}[] = "
                  f"{_c_bytes(case['wire_hex'])};\n")
        out.write(f"static const size_t s_wifi_invalid_{index}_len = "
                  f"{len(raw)};\n")
        out.write(f"static const int s_wifi_invalid_{index}_kind = "
                  f"{INVALID_KINDS[case['id']]};\n")
    out.write(f"static const size_t s_wifi_invalid_count = "
              f"{len(invalid)};\n")
    out.write("static const uint8_t *const s_wifi_invalid[] = {" +
              ", ".join(f"s_wifi_invalid_{i}"
                        for i in range(len(invalid))) + "};\n")
    out.write("static const size_t s_wifi_invalid_len[] = {" +
              ", ".join(f"s_wifi_invalid_{i}_len"
                        for i in range(len(invalid))) + "};\n")
    out.write("static const int s_wifi_invalid_kind[] = {" +
              ", ".join(f"s_wifi_invalid_{i}_kind"
                        for i in range(len(invalid))) + "};\n")

    out.write("\n/* wifi.v1 operation_results.json result payloads. */\n")
    for index, case in enumerate(op_results["cases"] + op_results["error_cases"]):
        raw = bytes.fromhex(case["result_payload_hex"])
        out.write(f"static const uint8_t s_wifi_result_{index}[] = "
                  f"{_c_bytes(case['result_payload_hex'])};\n")
        out.write(f"static const size_t s_wifi_result_{index}_len = "
                  f"{len(raw)};\n")
        out.write(f"static const uint8_t s_wifi_result_{index}_method = "
                  f"{case['method_id']};\n")
        out.write(f"static const int s_wifi_result_{index}_state = "
                  f"{STATE_VALUES[case['state']]};\n")
        out.write(f"static const int s_wifi_result_{index}_error = "
                  f"{STATUS_VALUES[case['error']]};\n")
        value = case.get("result_value") or {}
        out.write(f"static const uint64_t s_wifi_result_{index}_generation = "
                  f"{value.get('generation', 0)};\n")
        out.write(f"static const int s_wifi_result_{index}_wifi_state = "
                  f"{5 if value.get('state') == 'CONNECTED' else 1};\n")
        out.write(f"static const int s_wifi_result_{index}_failure = "
                  f"0;\n")
        ssid = bytes.fromhex(value.get("ssid", ""))
        out.write(f"static const uint8_t s_wifi_result_{index}_ssid[] = "
                  f"{_c_bytes(value.get('ssid', ''))};\n")
        out.write(f"static const size_t s_wifi_result_{index}_ssid_len = "
                  f"{len(ssid)};\n")
        out.write(f"static const bool s_wifi_result_{index}_has_ipv4 = "
                  f"{'true' if value.get('has_ipv4') else 'false'};\n")
        out.write(f"static const bool s_wifi_result_{index}_saved_profile = "
                  f"{'true' if value.get('saved_profile') else 'false'};\n")
        out.write(f"static const bool s_wifi_result_{index}_persisted = "
                  f"{'true' if value.get('profile_persisted') else 'false'};\n")
        out.write(f"static const bool s_wifi_result_{index}_auto_connect = "
                  f"{'true' if value.get('auto_connect') else 'false'};\n")
        out.write(f"static const bool s_wifi_result_{index}_manual_hold = "
                  f"{'true' if value.get('manual_hold') else 'false'};\n")
        out.write(f"static const uint64_t s_wifi_result_{index}_revision = "
                  f"{value.get('profile_revision', 0)};\n")
        out.write(f"static const uint64_t s_wifi_result_{index}_sync_id = "
                  f"{value.get('applied_client_sync_id', 0)};\n")
    out.write(f"static const size_t s_wifi_result_count = "
              f"{len(op_results['cases']) + len(op_results['error_cases'])};\n")
    result_count = len(op_results["cases"]) + len(op_results["error_cases"])
    out.write("static const uint8_t *const s_wifi_result[] = {" +
              ", ".join(f"s_wifi_result_{i}"
                        for i in range(result_count)) + "};\n")
    out.write("static const size_t s_wifi_result_len[] = {" +
              ", ".join(f"s_wifi_result_{i}_len"
                        for i in range(result_count)) + "};\n")
    out.write("static const uint8_t s_wifi_result_method[] = {" +
              ", ".join(f"s_wifi_result_{i}_method"
                        for i in range(result_count)) + "};\n")
    out.write("static const int s_wifi_result_state[] = {" +
              ", ".join(f"s_wifi_result_{i}_state"
                        for i in range(result_count)) + "};\n")
    out.write("static const int s_wifi_result_error[] = {" +
              ", ".join(f"s_wifi_result_{i}_error"
                        for i in range(result_count)) + "};\n")
    out.write("static const uint64_t s_wifi_result_generation[] = {" +
              ", ".join(f"s_wifi_result_{i}_generation"
                        for i in range(result_count)) + "};\n")
    out.write("static const int s_wifi_result_wifi_state[] = {" +
              ", ".join(f"s_wifi_result_{i}_wifi_state"
                        for i in range(result_count)) + "};\n")
    out.write("static const uint8_t *const s_wifi_result_ssid[] = {" +
              ", ".join(f"s_wifi_result_{i}_ssid"
                        for i in range(result_count)) + "};\n")
    out.write("static const size_t s_wifi_result_ssid_len[] = {" +
              ", ".join(f"s_wifi_result_{i}_ssid_len"
                        for i in range(result_count)) + "};\n")
    out.write("static const bool s_wifi_result_has_ipv4[] = {" +
              ", ".join(f"s_wifi_result_{i}_has_ipv4"
                        for i in range(result_count)) + "};\n")
    out.write("static const bool s_wifi_result_saved_profile[] = {" +
              ", ".join(f"s_wifi_result_{i}_saved_profile"
                        for i in range(result_count)) + "};\n")
    out.write("static const bool s_wifi_result_persisted[] = {" +
              ", ".join(f"s_wifi_result_{i}_persisted"
                        for i in range(result_count)) + "};\n")
    out.write("static const bool s_wifi_result_auto_connect[] = {" +
              ", ".join(f"s_wifi_result_{i}_auto_connect"
                        for i in range(result_count)) + "};\n")
    out.write("static const bool s_wifi_result_manual_hold[] = {" +
              ", ".join(f"s_wifi_result_{i}_manual_hold"
                        for i in range(result_count)) + "};\n")
    out.write("static const uint64_t s_wifi_result_revision[] = {" +
              ", ".join(f"s_wifi_result_{i}_revision"
                        for i in range(result_count)) + "};\n")
    out.write("static const uint64_t s_wifi_result_sync_id[] = {" +
              ", ".join(f"s_wifi_result_{i}_sync_id"
                        for i in range(result_count)) + "};\n")

    # --- wifi.v1 golden.json: the canonical SetCredentialsRequest. ---
    out.write("\n/* wifi.v1 golden.json. */\n")
    wifi_goldens = json.loads(
        (wifi_dir / "golden.json").read_text(encoding="utf-8"))
    wifi_golden = wifi_goldens[0]
    out.write(f"static const uint8_t s_wifi_golden[] = "
              f"{_c_bytes(wifi_golden['canonical_hex'])};\n")
    out.write(f"static const size_t s_wifi_golden_len = "
              f"{len(bytes.fromhex(wifi_golden['canonical_hex']))};\n")

    out.write("\n#endif /* CONTRACT_FIXTURES_WIFI_INC */\n")
    out.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--kind", choices=["core", "wifi"], default="core")
    args = parser.parse_args()
    if args.kind == "wifi":
        generate_wifi(args.fixtures, args.out)
    else:
        generate(args.fixtures, args.out)
    print(f"generated {args.out}")


if __name__ == "__main__":
    main()
