#!/usr/bin/env python3
"""Generate C vectors from the Device Link contract fixtures.

Consumes contracts/provisioning/fixtures/core/v2/{wire,framing,
authorization}.json and emits contract_fixtures.inc for the host test
test_contract_fixtures.c. The generated file is a build artifact; the
fixture checkout is the source of truth.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def _c_bytes(hex_text: str) -> str:
    raw = bytes.fromhex(hex_text)
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
    out.write("\n/* wire.json responses (status bytes). */\n")
    emit_group(out, "s_response_status",
               [{"hex": vector["status_hex"]} for vector in wire["responses"]],
               [])

    # --- wire.json: link errors (status value -> status_hex). ---
    out.write("\n/* wire.json link_errors (value, status_hex). */\n")
    emit_group(out, "s_link_error",
               [{"hex": vector["status_hex"], "value": vector["value"]}
                for vector in wire["link_errors"]], ["value"])

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

    out.write("\n#endif /* CONTRACT_FIXTURES_INC */\n")
    out.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    generate(args.fixtures, args.out)
    print(f"generated {args.out}")


if __name__ == "__main__":
    main()
