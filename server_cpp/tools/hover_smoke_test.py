import json
import os
import re
import subprocess
import sys
import time


def lsp_msg(obj):
    s = json.dumps(obj, separators=(",", ":"))
    b = s.encode("utf-8")
    hdr = ("Content-Length: %d\r\n\r\n" % len(b)).encode("utf-8")
    return hdr + b


def iter_lsp_frames(raw):
    pos = 0
    header_re = re.compile(br"Content-Length:\s*(\d+)\r\n\r\n")
    while True:
        m = header_re.search(raw, pos)
        if not m:
            break
        header_end = m.end()
        length = int(m.group(1))
        body = raw[header_end : header_end + length]
        yield body
        pos = header_end + length


def member_bullet_index(markdown, member_name):
    pattern = re.compile(r"- `[^`\n]*\b" + re.escape(member_name) + r"`")
    match = pattern.search(markdown or "")
    return -1 if match is None else match.start()


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    exe = os.path.join(repo, "server_cpp", "build", "nsf_lsp.exe")
    if not os.path.isfile(exe):
        sys.stderr.write("nsf_lsp.exe not found under server_cpp/build\n")
        return 1
    decls_path = os.path.join(repo, "test_files", "module_decls.nsf")
    decls_uri = "file:///" + decls_path.replace("\\", "/")
    main_path = os.path.join(repo, "test_files", "main.nsf")
    main_uri = "file:///" + main_path.replace("\\", "/")
    struct_path = os.path.join(repo, "test_files", "module_struct_completion.nsf")
    struct_uri = "file:///" + struct_path.replace("\\", "/")
    diag_path = os.path.join(repo, "test_files", "diagnostics_line_map.nsf")
    diag_uri = "file:///" + diag_path.replace("\\", "/")
    diag_ok_path = os.path.join(repo, "test_files", "diagnostics_smoke_ok.nsf")
    diag_ok_uri = "file:///" + diag_ok_path.replace("\\", "/")
    diag_invalid_suffix_path = os.path.join(repo, "test_files", "diagnostics_numeric_invalid_suffix.nsf")
    diag_invalid_suffix_uri = "file:///" + diag_invalid_suffix_path.replace("\\", "/")
    diag_struct_members_ok_path = os.path.join(
        repo, "test_files", "diagnostics_struct_members_across_structs_ok.nsf"
    )
    diag_struct_members_ok_uri = "file:///" + diag_struct_members_ok_path.replace("\\", "/")
    signature_path = os.path.join(repo, "test_files", "module_signature_help.nsf")
    signature_uri = "file:///" + signature_path.replace("\\", "/")
    external_path = sys.argv[1] if len(sys.argv) > 1 else None
    external_uri = None
    external_text = None
    if external_path:
        if not os.path.exists(external_path):
            sys.stderr.write("file not found: %s\n" % external_path)
            return 1
        external_uri = "file:///" + os.path.abspath(external_path).replace("\\", "/")
        external_text = open(external_path, "rb").read().decode("utf-8")

    decls_text = open(decls_path, "rb").read().decode("utf-8")
    main_text = open(main_path, "rb").read().decode("utf-8")
    struct_text = open(struct_path, "rb").read().decode("utf-8")
    diag_text = open(diag_path, "rb").read().decode("utf-8")
    diag_ok_text = open(diag_ok_path, "rb").read().decode("utf-8")
    diag_invalid_suffix_text = open(diag_invalid_suffix_path, "rb").read().decode("utf-8")
    diag_struct_members_ok_text = open(diag_struct_members_ok_path, "rb").read().decode("utf-8")
    signature_text = open(signature_path, "rb").read().decode("utf-8")
    main_line_190 = main_text.splitlines()[190]
    abs_char = main_line_190.find("abs(")
    if abs_char < 0:
        sys.stderr.write("abs() not found in main.nsf line 191\n")
        return 1

    init = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "processId": None,
            "rootUri": "file:///" + repo.replace("\\", "/"),
            "capabilities": {},
            "workspaceFolders": [{"uri": "file:///" + repo.replace("\\", "/"), "name": "repo"}],
        },
    }
    initialized = {"jsonrpc": "2.0", "method": "initialized", "params": {}}
    did_change_configuration = {
        "jsonrpc": "2.0",
        "method": "workspace/didChangeConfiguration",
        "params": {
            "settings": {
                "nsf": {
                    "semanticCache": {
                        "shadowCompare": {
                            "enabled": True
                        }
                    }
                }
            }
        },
    }
    did_open_decls = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": decls_uri,
                "languageId": "nsf",
                "version": 1,
                "text": decls_text,
            }
        },
    }
    did_open_main = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {"uri": main_uri, "languageId": "nsf", "version": 1, "text": main_text}
        },
    }
    did_open_struct = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": struct_uri,
                "languageId": "nsf",
                "version": 1,
                "text": struct_text,
            }
        },
    }
    did_open_diag = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": diag_uri,
                "languageId": "nsf",
                "version": 1,
                "text": diag_text,
            }
        },
    }
    did_open_diag_ok = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": diag_ok_uri,
                "languageId": "nsf",
                "version": 1,
                "text": diag_ok_text,
            }
        },
    }
    did_open_diag_invalid_suffix = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": diag_invalid_suffix_uri,
                "languageId": "nsf",
                "version": 1,
                "text": diag_invalid_suffix_text,
            }
        },
    }
    did_open_diag_struct_members_ok = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": diag_struct_members_ok_uri,
                "languageId": "nsf",
                "version": 1,
                "text": diag_struct_members_ok_text,
            }
        },
    }
    did_open_signature = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": signature_uri,
                "languageId": "nsf",
                "version": 1,
                "text": signature_text,
            }
        },
    }
    hover_local = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": decls_uri}, "position": {"line": 9, "character": 12}},
    }
    hover_abs = {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": main_uri}, "position": {"line": 190, "character": abs_char + 1}},
    }
    hover_struct = {
        "jsonrpc": "2.0",
        "id": 4,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": struct_uri}, "position": {"line": 0, "character": 7}},
    }
    definition_local = {
        "jsonrpc": "2.0",
        "id": 7,
        "method": "textDocument/definition",
        "params": {"textDocument": {"uri": decls_uri}, "position": {"line": 9, "character": 23}},
    }
    signature_help_local = {
        "jsonrpc": "2.0",
        "id": 8,
        "method": "textDocument/signatureHelp",
        "params": {"textDocument": {"uri": signature_uri}, "position": {"line": 7, "character": 24}},
    }
    did_open_external = None
    hover_vs_input = None
    hover_ps_input = None
    if external_uri and external_text is not None:
        did_open_external = {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": external_uri,
                    "languageId": "nsf",
                    "version": 1,
                    "text": external_text,
                }
            },
        }
        hover_vs_input = {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "textDocument/hover",
            "params": {"textDocument": {"uri": external_uri}, "position": {"line": 0, "character": 10}},
        }
        hover_ps_input = {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "textDocument/hover",
            "params": {"textDocument": {"uri": external_uri}, "position": {"line": 7, "character": 10}},
        }

    payload = b"".join(
        [
            lsp_msg(init),
            lsp_msg(initialized),
            lsp_msg(did_change_configuration),
            lsp_msg(did_open_decls),
            lsp_msg(did_open_main),
            lsp_msg(did_open_struct),
            lsp_msg(did_open_diag),
            lsp_msg(did_open_diag_ok),
            lsp_msg(did_open_diag_invalid_suffix),
            lsp_msg(did_open_diag_struct_members_ok),
            lsp_msg(did_open_signature),
            lsp_msg(hover_local),
            lsp_msg(hover_abs),
            lsp_msg(hover_struct),
            lsp_msg(definition_local),
            lsp_msg(signature_help_local),
        ]
    )
    if did_open_external and hover_vs_input:
        payload += b"".join([lsp_msg(did_open_external), lsp_msg(hover_vs_input), lsp_msg(hover_ps_input)])

    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p.stdin.write(payload)
    p.stdin.flush()
    time.sleep(4.0)
    try:
        p.stdin.close()
    except Exception:
        pass
    out = p.stdout.read()
    err = p.stderr.read()
    p.wait()

    want = {2: False, 3: False, 4: False, 7: False, 8: False}
    if external_uri:
        want[5] = False
        want[6] = False
    struct_ok = False
    external_ok = False
    external_ps_ok = False
    diag_external = []
    diag_internal = []
    diag_internal_ok = []
    diag_internal_invalid_suffix = []
    diag_internal_struct_members_ok = []
    got_diag_internal_ok = False
    got_diag_internal_invalid_suffix = False
    got_diag_internal_struct_members_ok = False
    definition_ok = False
    signature_help_ok = False
    shadow_compare_messages = []
    signature_help_shadow_compare_messages = []
    for body in iter_lsp_frames(out):
        if b"\"method\":\"textDocument/publishDiagnostics\"" in body:
            try:
                msg = json.loads(body.decode("utf-8", "ignore"))
                params = msg.get("params", {})
                if external_uri and params.get("uri") == external_uri:
                    diag_external = params.get("diagnostics", [])
                if params.get("uri") == diag_uri:
                    diag_internal = params.get("diagnostics", [])
                if params.get("uri") == diag_ok_uri:
                    diag_internal_ok = params.get("diagnostics", [])
                    got_diag_internal_ok = True
                if params.get("uri") == diag_invalid_suffix_uri:
                    diag_internal_invalid_suffix = params.get("diagnostics", [])
                    got_diag_internal_invalid_suffix = True
                if params.get("uri") == diag_struct_members_ok_uri:
                    diag_internal_struct_members_ok = params.get("diagnostics", [])
                    got_diag_internal_struct_members_ok = True
            except Exception:
                pass
        if b"\"method\":\"window/logMessage\"" in body:
            try:
                msg = json.loads(body.decode("utf-8", "ignore"))
                params = msg.get("params", {})
                message = params.get("message", "")
                if isinstance(message, str) and message.startswith(
                    "nsf definition shadowCompare mismatch:"
                ):
                    shadow_compare_messages.append(message)
                if isinstance(message, str) and message.startswith(
                    "nsf signatureHelp shadowCompare mismatch:"
                ):
                    signature_help_shadow_compare_messages.append(message)
            except Exception:
                pass
        if (
            b"\"id\":2" in body
            or b"\"id\":3" in body
            or b"\"id\":4" in body
            or b"\"id\":5" in body
            or b"\"id\":6" in body
            or b"\"id\":7" in body
            or b"\"id\":8" in body
        ):
            sys.stdout.write(body.decode("utf-8"))
            sys.stdout.write("\n")
            if b"\"id\":2" in body:
                want[2] = True
            if b"\"id\":3" in body:
                want[3] = True
            if b"\"id\":4" in body:
                want[4] = True
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    md = obj.get("result", {}).get("contents", {}).get("value", "")
                except Exception:
                    md = ""
                i = member_bullet_index(md, "color")
                j = member_bullet_index(md, "value")
                if i >= 0 and j >= 0 and i < j:
                    struct_ok = True
            if b"\"id\":5" in body:
                want[5] = True
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    md = obj.get("result", {}).get("contents", {}).get("value", "")
                except Exception:
                    md = ""
                i = member_bullet_index(md, "position")
                j = member_bullet_index(md, "instance_id")
                if i >= 0 and j >= 0 and i < j:
                    external_ok = True
            if b"\"id\":6" in body:
                want[6] = True
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    md = obj.get("result", {}).get("contents", {}).get("value", "")
                except Exception:
                    md = ""
                i = member_bullet_index(md, "final_position")
                j = member_bullet_index(md, "instance_id")
                if i >= 0 and j >= 0 and i < j:
                    external_ps_ok = True
            if b"\"id\":7" in body:
                want[7] = True
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    result = obj.get("result", [])
                    if isinstance(result, list) and len(result) > 0:
                        location = result[0]
                        uri = location.get("uri", "")
                        line = (
                            location.get("range", {})
                            .get("start", {})
                            .get("line")
                        )
                        if uri == decls_uri and line == 2:
                            definition_ok = True
                except Exception:
                    pass
            if b"\"id\":8" in body:
                want[8] = True
                if b"SigTarget(" in body:
                    signature_help_ok = True
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    result = obj.get("result", {})
                    signatures = result.get("signatures", [])
                    if isinstance(signatures, list) and len(signatures) > 0:
                        label = signatures[0].get("label", "")
                        if isinstance(label, str) and "SigTarget(" in label:
                            signature_help_ok = True
                except Exception:
                    pass
    if want[2] and want[3]:
        if (
            want[4]
            and want[7]
            and want[8]
            and struct_ok
            and definition_ok
            and signature_help_ok
            and (not external_uri or (want.get(5) and external_ok))
        ):
            expected_line = 4
            ok_line_map = False
            for d in diag_internal:
                if d.get("message", "").startswith("Assignment type mismatch"):
                    s = d.get("range", {}).get("start", {})
                    if s.get("line") == expected_line:
                        ok_line_map = True
                        break
            if not ok_line_map:
                sys.stderr.write("diagnostics line mapping check failed\n")
                return 1
            ok_add = False
            ok_cmp = False
            ok_assign = False
            for d in diag_internal:
                s = d.get("range", {}).get("start", {})
                line = s.get("line")
                msg = d.get("message", "")
                if msg.startswith("Binary operator type mismatch") and line == 7:
                    ok_add = True
                if msg.startswith("Binary operator type mismatch") and line == 8:
                    ok_cmp = True
                if msg.startswith("Assignment type mismatch: float3 = float2.") and line == 9:
                    ok_assign = True
            if not (ok_add and ok_cmp and ok_assign):
                sys.stderr.write("diagnostics operator/assignment checks failed\n")
                return 1
            if not got_diag_internal_invalid_suffix:
                sys.stderr.write("numeric invalid suffix diagnostics check failed\n")
                sys.stderr.write("got=%s\n" % json.dumps(diag_internal_invalid_suffix))
                return 1
            if len(diag_internal_invalid_suffix) < 1:
                sys.stderr.write("numeric invalid suffix diagnostics check failed\n")
                sys.stderr.write("got=%s\n" % json.dumps(diag_internal_invalid_suffix))
                return 1
            for d in diag_internal_invalid_suffix:
                if not d.get("message", "").startswith("Invalid numeric literal suffix: "):
                    sys.stderr.write("numeric invalid suffix diagnostics check failed\n")
                    sys.stderr.write("got=%s\n" % json.dumps(diag_internal_invalid_suffix))
                    return 1
            if not got_diag_internal_struct_members_ok:
                sys.stderr.write("struct member duplicate diagnostics check failed\n")
                return 1
            if any(
                d.get("message", "").startswith("Duplicate global declaration:")
                for d in diag_internal_struct_members_ok
            ):
                sys.stderr.write("struct member duplicate diagnostics check failed\n")
                sys.stderr.write("got=%s\n" % json.dumps(diag_internal_struct_members_ok))
                return 1
            for message in shadow_compare_messages:
                if " newPath=" not in message or " oldPath=" not in message:
                    sys.stderr.write("definition shadow compare message format check failed\n")
                    sys.stderr.write("got=%s\n" % json.dumps(shadow_compare_messages))
                    return 1
            for message in signature_help_shadow_compare_messages:
                if " newPath=" not in message or " oldPath=" not in message:
                    sys.stderr.write("signatureHelp shadow compare message format check failed\n")
                    sys.stderr.write("got=%s\n" % json.dumps(signature_help_shadow_compare_messages))
                    return 1
            if external_uri and diag_external:
                sys.stdout.write("diagnostics(external):\n")
                interesting = set(list(range(240, 320)) + list(range(360, 390)))
                for d in diag_external:
                    r = d.get("range", {})
                    s = r.get("start", {})
                    line = s.get("line")
                    if line not in interesting:
                        continue
                    msg = d.get("message", "")
                    sev = d.get("severity", "")
                    sys.stdout.write("  line=%s severity=%s %s\n" % (line + 1, sev, msg))
            return 0
        if want[4] and not struct_ok:
            sys.stderr.write("struct member order check failed\n")
            return 1
        if want[7] and not definition_ok:
            sys.stderr.write("definition result check failed\n")
            return 1
        if want[8] and not signature_help_ok:
            sys.stderr.write("signatureHelp result check failed\n")
            return 1
        if external_uri and want.get(5) and not external_ok:
            sys.stderr.write("external VS_INPUT member check failed\n")
            return 1
        if external_uri and want.get(6) and not external_ps_ok:
            sys.stderr.write("external PS_INPUT member check failed\n")
            return 1
    sys.stderr.write("hover response not found for: %s\n" % [k for k, v in want.items() if not v])
    sys.stderr.write(err.decode("utf-8", "ignore"))
    return 1


if __name__ == "__main__":
    sys.exit(main())

