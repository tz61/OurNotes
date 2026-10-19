#!/usr/bin/env python3
"""Generate resources/schema.json from a Zotero database.

Usage: python3 tools/gen_schema.py ~/Zotero/zotero.sqlite resources/schema.json

The Zotero database is copied to a temporary directory first (Zotero keeps it
locked while running), so this never touches the original file.
"""
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import zlib


def main(src, dst):
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "zotero.sqlite")
        shutil.copy2(src, db)
        if os.path.exists(src + "-wal"):
            shutil.copy2(src + "-wal", db + "-wal")
        c = sqlite3.connect(db)
        types = []
        for type_id, name in c.execute(
            "SELECT itemTypeID, typeName FROM itemTypes WHERE display>0 ORDER BY itemTypeID"
        ):
            fields = []
            for fname, base in c.execute(
                """SELECT f.fieldName,
                          (SELECT bf.fieldName FROM baseFieldMappings b
                           JOIN fields bf ON bf.fieldID=b.baseFieldID
                           WHERE b.itemTypeID=itf.itemTypeID AND b.fieldID=itf.fieldID)
                   FROM itemTypeFields itf JOIN fields f USING(fieldID)
                   WHERE itf.itemTypeID=? ORDER BY itf.orderIndex""",
                (type_id,),
            ):
                entry = {"field": fname}
                if base:
                    entry["baseField"] = base
                fields.append(entry)
            creators = [
                {"creatorType": ct, **({"primary": True} if prim else {})}
                for ct, prim in c.execute(
                    """SELECT ct.creatorType, itc.primaryField FROM itemTypeCreatorTypes itc
                       JOIN creatorTypes ct USING(creatorTypeID)
                       WHERE itc.itemTypeID=? ORDER BY itc.primaryField DESC, ct.creatorTypeID""",
                    (type_id,),
                )
            ]
            types.append({"itemType": name, "fields": fields, "creatorTypes": creators})
        blob = c.execute("SELECT value FROM settings WHERE setting='globalSchema' AND key='data'").fetchone()
        glob = json.loads(zlib.decompress(blob[0])) if blob else {}
        out = {
            "version": glob.get("version"),
            "itemTypes": types,
            "locales": {"en-US": glob.get("locales", {}).get("en-US", {})},
            "csl": glob.get("csl", {}),
        }
    with open(dst, "w") as f:
        json.dump(out, f, indent=1, sort_keys=False)
    print(f"wrote {dst}: {len(types)} item types")


if __name__ == "__main__":
    main(os.path.expanduser(sys.argv[1]), sys.argv[2])
