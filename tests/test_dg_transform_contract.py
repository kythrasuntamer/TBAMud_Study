#!/usr/bin/env python3
"""Guard otransform/mtransform: every per-instance link survives the swap.

The transform copies the new prototype's data over the running entity and
then restores the fields that belong to the instance. A field missing from
that list is silently replaced by the temporary copy's value.
"""
from pathlib import Path
import sys

objcmd = Path(sys.argv[1]).read_text(encoding="utf-8")
body = objcmd[objcmd.index("static OCMD(do_otransform)"):]
body = body[:body.index("memcpy(obj, &tmpobj, sizeof(*obj));")]
for field in ("in_room", "carried_by", "worn_by", "worn_on", "in_obj",
              "contains", "script_id", "proto_script", "script",
              "next_content", "next", "sitting_here", "events"):
    assert f"tmpobj.{field} = " in body, "otransform keeps " + field
mobcmd = Path(sys.argv[2]).read_text(encoding="utf-8")
body = mobcmd[mobcmd.index("ACMD(do_mtransform)"):]
body = body[:body.index("memcpy(ch, &tmpmob, sizeof(*ch));")]
for field in ("script_id", "affected", "carrying", "proto_script", "script",
              "memory", "events", "next_in_room", "next", "next_fighting",
              "followers", "master", "group"):
    assert f"tmpmob.{field} = ch->{field};" in body, "mtransform keeps " + field
for macro in ("SITTING", "NEXT_SITTING", "GET_POS", "FIGHTING", "HUNTING"):
    assert f"{macro}(&tmpmob) = {macro}(ch);" in body, "mtransform keeps " + macro
print("transform contract verified")
