#!/usr/bin/env python3
# =============================================================================
# generator.py — ITCH 5.0 Zero-Copy C++20 Parser Code Generator
# =============================================================================
#
# Ingests itch50_schema.json and emits generated_itch_parser.hpp containing:
#
#   1. #pragma pack(push, 1) structs with exact wire-layout fields
#   2. Inline accessor methods — endian-aware for integers, zero-copy
#      std::string_view for alpha fields, implied-decimal double helpers
#      for price fields
#   3. static_assert(sizeof(T) == N) for every struct
#   4. constexpr message_size() and message_name() lookup functions
#   5. msg:: namespace with char constants for every message type
#
# Usage:
#   python generator.py schema/itch50_schema.json generated/generated_itch_parser.hpp
#
# =============================================================================

import json
import sys
from pathlib import Path
from datetime import datetime, timezone


# ─── Schema Type → C++ Mapping ──────────────────────────────────────────────
#
# For each schema type we define:
#   storage  : C++ type used for the raw struct field
#   array    : if > 0, the field is declared as storage[array]
#   ret      : C++ return type of the accessor method
#   reader   : detail:: function to call (None = direct return)
#   decimals : implied decimal places for price types (optional)
#
TYPE_INFO = {
    "alpha":     {"storage": "char",     "array": 0, "ret": "char",              "reader": None},
    "alpha_2":   {"storage": "char",     "array": 2, "ret": "std::string_view",  "reader": None},
    "alpha_4":   {"storage": "char",     "array": 4, "ret": "std::string_view",  "reader": None},
    "alpha_8":   {"storage": "char",     "array": 8, "ret": "std::string_view",  "reader": None},
    "alpha_14":  {"storage": "char",     "array": 14,"ret": "std::string_view",  "reader": None},
    "integer_2": {"storage": "uint16_t", "array": 0, "ret": "uint16_t",          "reader": "read_be16"},
    "integer_4": {"storage": "uint32_t", "array": 0, "ret": "uint32_t",          "reader": "read_be32"},
    "integer_6": {"storage": "uint8_t",  "array": 6, "ret": "uint64_t",          "reader": "read_be48"},
    "integer_8": {"storage": "uint64_t", "array": 0, "ret": "uint64_t",          "reader": "read_be64"},
    "price_4":   {"storage": "uint32_t", "array": 0, "ret": "uint32_t",          "reader": "read_be32", "decimals": 4},
    "price_8":   {"storage": "uint64_t", "array": 0, "ret": "uint64_t",          "reader": "read_be64", "decimals": 8},
}

# Multiplier constants for converting fixed-point price to double
PRICE_MULTIPLIER = {4: "0.0001", 8: "0.00000001"}


def emit_field_declaration(field: dict) -> str:
    """Generate the raw struct field declaration line."""
    info = TYPE_INFO[field["type"]]
    name = field["name"]

    if info["array"] > 0:
        return f'    {info["storage"]:<10s} {name}_[{info["array"]}];'
    else:
        return f'    {info["storage"]:<10s} {name}_;'


def emit_accessors(field: dict) -> list[str]:
    """Generate zero-copy accessor method(s) for a single field.

    Returns a list of C++ method declaration strings (indented 4 spaces).
    """
    info = TYPE_INFO[field["type"]]
    ftype = field["type"]
    name = field["name"]
    lines: list[str] = []

    # ── Alpha (single char) ──────────────────────────────────────────────
    if ftype == "alpha":
        lines.append(
            f'    [[nodiscard]] char {name}() const noexcept '
            f'{{ return {name}_; }}'
        )

    # ── Alpha array (fixed-length string) ────────────────────────────────
    elif ftype.startswith("alpha_"):
        n = info["array"]
        lines.append(
            f'    [[nodiscard]] std::string_view {name}() const noexcept '
            f'{{ return {{{name}_, {n}}}; }}'
        )
        # Convenience: trimmed version strips trailing ITCH space-padding
        lines.append(
            f'    [[nodiscard]] std::string_view {name}_trimmed() const noexcept '
            f'{{ return parser_utils::detail::trim_right({name}()); }}'
        )

    # ── 48-bit timestamp (uint8_t[6] → uint64_t) ────────────────────────
    elif ftype == "integer_6":
        lines.append(
            f'    [[nodiscard]] uint64_t {name}() const noexcept '
            f'{{ return parser_utils::detail::read_be48(reinterpret_cast<const char*>({name}_)); }}'
        )

    # ── Standard integers (16/32/64) ─────────────────────────────────────
    elif ftype in ("integer_2", "integer_4", "integer_8"):
        reader = info["reader"]
        ret = info["ret"]
        lines.append(
            f'    [[nodiscard]] {ret} {name}() const noexcept '
            f'{{ return parser_utils::detail::{reader}(reinterpret_cast<const char*>(&{name}_)); }}'
        )

    # ── Price (integer + implied decimal conversion) ─────────────────────
    elif ftype in ("price_4", "price_8"):
        reader = info["reader"]
        ret = info["ret"]
        decimals = info["decimals"]
        mult = PRICE_MULTIPLIER[decimals]

        # Primary accessor: returns raw integer (for order book math)
        lines.append(
            f'    [[nodiscard]] {ret} {name}() const noexcept '
            f'{{ return parser_utils::detail::{reader}(reinterpret_cast<const char*>(&{name}_)); }}'
        )
        # Convenience: double with implied decimals (for display/logging — NOT hot path)
        lines.append(
            f'    [[nodiscard]] double {name}_double() const noexcept '
            f'{{ return static_cast<double>({name}()) * {mult}; }}'
        )

    return lines


def generate(schema_path: str, output_path: str) -> None:
    """Main generation pipeline: schema JSON → C++20 header."""
    with open(schema_path, 'r', encoding="utf-8") as f:
        schema = json.load(f)

    messages = schema["messages"]
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    total_fields = sum(len(m["fields"]) for m in messages)
    namespace = schema.get("namespace", "protocol_parser")
    protocol_name = schema.get("protocol", "Unknown Protocol")

    out: list[str] = []

    # ─── File Header ─────────────────────────────────────────────────────
    out.append("#pragma once")
    out.append(f"// {Path(output_path).name}")
    out.append("//")
    out.append("// AUTO-GENERATED — DO NOT EDIT MANUALLY")
    out.append(f"// Generated at : {now}")
    out.append(f"// Protocol     : {schema['protocol']}")
    out.append(f"// Byte order   : {schema['byte_order']}")
    out.append(f"// Messages     : {len(messages)}")
    out.append(f"// Total fields : {total_fields}")
    out.append("//")
    out.append(f"// Zero-copy, zero-allocation {protocol_name} parser.")
    out.append("// Auto-generated by generator.py.")
    out.append("// Structs are #pragma pack(1) to match exact wire layout.")
    out.append("// Use reinterpret_cast<const T*>(buffer) for zero-copy overlay.")
    out.append("// All multi-byte integers are converted from big-endian (network")
    out.append("// byte order) to host byte order via the detail:: helpers in")
    out.append("// endian_utils.hpp.  Alpha fields return std::string_view pointing")
    out.append("// directly into the source buffer (zero-copy).")
    out.append("// " + "=" * 77)
    out.append("")
    out.append('#include "endian_utils.hpp"')
    out.append("")
    out.append("#include <cstdint>")
    out.append("#include <cstddef>")
    out.append("#include <string_view>")
    out.append("")
    out.append(f"namespace {namespace} {{")
    out.append("")

    # ─── Message Type Constants ──────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Message Type Constants")
    out.append("// " + "─" * 77)
    out.append("namespace msg {")
    for msg in messages:
        pad = 40 - len(msg["name"])
        out.append(f"    inline constexpr char {msg['name']}{' ' * pad} = '{msg['type']}';")
    out.append("} // namespace msg")
    out.append("")

    # ─── Compile-time Message Size Lookup ─────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Compile-time message size lookup (O(1) via switch)")
    out.append("// Returns 0 for unknown message types.")
    out.append("// " + "─" * 77)
    out.append("[[nodiscard]] constexpr std::size_t message_size(char type) noexcept {")
    out.append("    switch (type) {")
    for msg in messages:
        out.append(f"        case '{msg['type']}': return {msg['size']:3d};  // {msg['name']}")
    out.append("        default:      return   0;")
    out.append("    }")
    out.append("}")
    out.append("")

    # ─── Message Name Lookup (debug / logging) ───────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Message name lookup (for logging/debug — not on the hot path)")
    out.append("// " + "─" * 77)
    out.append('[[nodiscard]] constexpr const char* message_name(char type) noexcept {')
    out.append("    switch (type) {")
    for msg in messages:
        out.append(f'        case \'{msg["type"]}\': return "{msg["name"]}";')
    out.append('        default:      return "Unknown";')
    out.append("    }")
    out.append("}")
    out.append("")

    # ─── Packed Message Structs ──────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Packed Message Structs")
    out.append("//")
    out.append("// #pragma pack(push, 1) ensures zero padding between fields,")
    out.append("// making sizeof(T) == wire_size for every struct.")
    out.append("// Overlay onto a raw buffer via:")
    out.append("//     auto* msg = reinterpret_cast<const AddOrder*>(buf);")
    out.append("// " + "─" * 77)
    out.append("")
    out.append("#pragma pack(push, 1)")
    out.append("")

    for msg in messages:
        name   = msg["name"]
        size   = msg["size"]
        mtype  = msg["type"]
        desc   = msg.get("description", "")

        # Struct comment header
        separator = "─" * (74 - len(name) - len(str(size)) - len(mtype))
        out.append(f"// ── {name} ('{mtype}', {size}B) {separator}")
        if desc:
            # Wrap long descriptions
            max_desc_len = 76
            if len(desc) > max_desc_len:
                words = desc.split()
                line = "//"
                for word in words:
                    if len(line) + len(word) + 1 > max_desc_len:
                        out.append(line)
                        line = "// " + word
                    else:
                        line += " " + word
                if line.strip("/ "):
                    out.append(line)
            else:
                out.append(f"// {desc}")

        out.append(f"struct {name} {{")

        # ── Raw field declarations ───────────────────────────────────────
        for field in msg["fields"]:
            decl = emit_field_declaration(field)
            # Annotate the message_type field with its expected value
            if field["name"] == "message_type":
                decl += f"  // always '{mtype}'"
            out.append(decl)

        out.append("")
        out.append("    // ── Accessors (zero-copy, endian-aware) ──")

        # ── Accessor methods ─────────────────────────────────────────────
        for field in msg["fields"]:
            for accessor_line in emit_accessors(field):
                out.append(accessor_line)

        out.append("};")
        out.append(
            f'static_assert(sizeof({name}) == {size}, '
            f'"sizeof({name}) must match wire size {size}");'
        )
        out.append("")

    out.append("#pragma pack(pop)")
    out.append("")

    # ─── Static Dispatcher ────────────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Concept for a message handler")
    out.append("// " + "─" * 77)
    out.append("template<typename Handler, typename Message>")
    out.append("concept CanHandle = requires(Handler& h, const Message& msg) {")
    out.append("    h.handle(msg);")
    out.append("};")
    out.append("")
    out.append("// " + "─" * 77)
    out.append("// Switch-based Dispatcher (Static Dispatch)")
    out.append("//")
    out.append("// Dispatches a raw buffer to the appropriate strongly-typed handle() method")
    out.append("// on the provided Handler instance. Only instantiates calls for messages")
    out.append("// the handler actually supports (via the CanHandle concept).")
    out.append("//")
    out.append("// Returns true if dispatched successfully, false if the message type is unknown.")
    out.append("// " + "─" * 77)
    out.append("template<typename Handler>")
    out.append("inline bool dispatch_message(Handler& handler, char type, const char* buffer) {")
    out.append("    switch(type) {")
    for msg in messages:
        t = msg["type"]
        n = msg["name"]
        out.append(f"        case '{t}':")
        out.append(f"            if constexpr (CanHandle<Handler, {n}>) {{")
        out.append(f"                handler.handle(*reinterpret_cast<const {n}*>(buffer));")
        out.append("            }")
        out.append("            break;")
    out.append("        default: return false;")
    out.append("    }")
    out.append("    return true;")
    out.append("}")
    out.append("")

    # ─── O(1) Dispatch Table ─────────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// O(1) Function Pointer Dispatch Table")
    out.append("//")
    out.append("// For absolute lowest latency, avoiding jump table bounds checks or")
    out.append("// branch mispredictions. Creates a 256-entry array of function pointers.")
    out.append("// " + "─" * 77)
    out.append("template<typename Handler>")
    out.append("struct DispatchTable {")
    out.append("    using FuncPtr = void(*)(Handler&, const char*);")
    out.append("    FuncPtr table[256]{};")
    out.append("")
    out.append("    constexpr DispatchTable() {")
    for msg in messages:
        t = msg["type"]
        n = msg["name"]
        out.append(f"        if constexpr (CanHandle<Handler, {n}>) {{")
        out.append(f"            table[static_cast<uint8_t>('{t}')] = [](Handler& h, const char* buf) {{")
        out.append(f"                h.handle(*reinterpret_cast<const {n}*>(buf));")
        out.append(f"            }};")
        out.append(f"        }}")
    out.append("    }")
    out.append("")
    out.append("    // Call this for O(1) dispatch")
    out.append("    inline bool dispatch(Handler& handler, char type, const char* buffer) const {")
    out.append("        if (auto func = table[static_cast<uint8_t>(type)]) {")
    out.append("            func(handler, buffer);")
    out.append("            return true;")
    out.append("        }")
    out.append("        return false;")
    out.append("    }")
    out.append("};")
    out.append("")

    # ─── Stream Parser ───────────────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// ─── Stream Parsing ─────────────────────────────────────────────────────────")
    out.append("// Loops through a contiguous buffer of length-prefixed messages.")
    out.append("// In common protocols, each message is often prefixed by a 2-byte length.")
    out.append("// Returns the number of bytes successfully consumed.")
    out.append("// " + "─" * 77)
    out.append("template<typename Handler>")
    out.append("inline std::size_t parse_stream(Handler& handler, const char* buffer, std::size_t length) {")
    out.append("    std::size_t offset = 0;")
    out.append("    while (length - offset >= 2) {")
    out.append("        // Read 2-byte message length (big-endian)")
    out.append("        uint16_t msg_len = parser_utils::detail::read_be16(buffer + offset);")
    out.append("        if (msg_len == 0) break; // A message must contain a type byte")
    out.append("        if (msg_len > length - offset - 2) break; // Incomplete message")
    out.append("")
    out.append("        // The first byte of the message is the message type")
    out.append("        char type = buffer[offset + 2];")
    out.append("        dispatch_message(handler, type, buffer + offset + 2);")
    out.append("        ")
    out.append("        offset += 2 + msg_len;")
    out.append("    }")
    out.append("    return offset;")
    out.append("}")
    out.append("")

    # ─── All Message Types Array ─────────────────────────────────────────
    out.append("// " + "─" * 77)
    out.append("// Enumeration of all defined message type characters")
    out.append("// " + "─" * 77)
    type_chars = ", ".join([f"'{m['type']}'" for m in messages])
    out.append(f"inline constexpr char ALL_MESSAGE_TYPES[] = {{ {type_chars} }};")
    out.append(f"inline constexpr std::size_t NUM_MESSAGE_TYPES = {len(messages)};")
    out.append("")

    # ─── Largest message size (useful for buffer pre-allocation) ──────────
    max_size = max(m["size"] for m in messages)
    out.append(f"// Largest single message in the protocol ({max_size}B)")
    out.append(f"inline constexpr std::size_t MAX_MESSAGE_SIZE = {max_size};")
    out.append("")

    out.append(f"}} // namespace {namespace}")
    out.append("")

    # ─── Write Output ────────────────────────────────────────────────────
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join(out)
    output.write_text(content, encoding="utf-8")

    # ─── Summary ─────────────────────────────────────────────────────────
    print(f"[generator] [OK] Generated {output}")
    print(f"[generator]   Protocol     : {schema['protocol']}")
    print(f"[generator]   Messages     : {len(messages)}")
    print(f"[generator]   Total fields : {total_fields}")
    print(f"[generator]   C++ lines    : {len(out)}")
    print(f"[generator]   Max msg size : {max_size}B")
    print(f"[generator]   Output size  : {len(content)} bytes")


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: python generator.py <schema.json> <output.hpp>")
        print()
        print("Example:")
        print("  python generator/generator.py schema/itch50_schema.json generated/generated_itch_parser.hpp")
        sys.exit(1)

    generate(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
