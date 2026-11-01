"""Merge retained evidence without replacing or weakening existing contracts."""
from copy import deepcopy


def merge_contracts(base, extra):
    if base["reference_sha256"] != extra["reference_sha256"]:
        raise ValueError("different reference executable SHA-256")
    result = deepcopy(base)
    by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
    if len(by_key) != len(result["entries"]) + len(result["witnesses"]):
        raise ValueError("duplicate base contract identity")
    for kind in ("entries", "witnesses"):
        for incoming in extra[kind]:
            old = by_key.get(incoming["rva"])
            if old is None:
                old = deepcopy(incoming)
                result[kind].append(old)
                by_key[old["rva"]] = old
            else:
                for field in ("start", "bytes", "mask", "refs"):
                    if old[field] != incoming[field]:
                        raise ValueError(f"conflicting contract {old['rva']:#x}: {field}")
                old["groups"] = list(dict.fromkeys(old["groups"] + incoming["groups"]))
    # A former witness may become a primary. Keep one identity and one scan.
    promoted = [e for e in result["witnesses"] if e["groups"]]
    result["entries"].extend(promoted)
    result["witnesses"] = [e for e in result["witnesses"] if not e["groups"]]
    tables = {t["rva"]: t for t in result["tables"]}
    for incoming in extra["tables"]:
        old = tables.get(incoming["rva"])
        if old is None:
            old = deepcopy(incoming)
            result["tables"].append(old)
            tables[old["rva"]] = old
        else:
            if old["type"] != incoming["type"]:
                raise ValueError("conflicting RTTI identity")
            slots = {s["slot"]: s["target"] for s in old["slots"]}
            for slot in incoming["slots"]:
                if slot["slot"] in slots:
                    if slots[slot["slot"]] != slot["target"]:
                        raise ValueError("conflicting vtable slot")
                else:
                    old["slots"].append(deepcopy(slot))
                    slots[slot["slot"]] = slot["target"]
            old["groups"] = list(dict.fromkeys(
                old.get("groups", [old["group"]]) + incoming.get("groups", [incoming["group"]])))
    return result
