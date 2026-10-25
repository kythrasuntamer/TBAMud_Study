#!/usr/bin/env python3
"""Validate class and skill-tree import manifests before touching player saves.

No gameplay code is generated. Unknown prerequisites and cycles are errors.
"""
import argparse
import json
from pathlib import Path
import sys

def validate(manifest):
    issues=[]
    existing=manifest.get("existing",[])
    added=manifest.get("candidates",[])
    ids=[x.get("id") for x in existing+added]
    names=[x.get("name") for x in existing+added]
    if len(ids)!=len(set(ids)) or len(names)!=len(set(names)):
        issues.append("duplicate class id or class name")
    if [(x.get("id"),x.get("name")) for x in existing]!=[
        (0,"Wizard"),(1,"Cleric"),(2,"Thief"),(3,"Warrior")]:
        issues.append("existing player class IDs changed")
    if any(not isinstance(v,int) or v<0 or v>=32 for v in ids):
        issues.append("class id out of supported 0-31 bitvector range")
    if manifest.get("max_mortal_level")!=50 or manifest.get("immortal_start")!=51:
        issues.append("level boundaries must match levels 1-50 and 51-60")
    trees=manifest.get("skill_trees",[])
    byname={}
    for tree in trees:
        name=tree.get("class")
        if name not in names or name in byname:
            issues.append(f"unknown or duplicate skill tree class: {name}")
        byname[name]=tree
        skills=tree.get("skills",[])
        byskill={}
        for skill in skills:
            key=skill.get("key")
            if not isinstance(key,str) or not key or key in byskill:
                issues.append(f"duplicate or invalid skill key {key!r} for {name}")
            byskill[key]=skill
            if skill.get("unlock") not in ("trainer", "quest", "challenge", "boss"):
                issues.append(f"missing or invalid unlock route for {name}/{key}")
            if skill.get("implementation") not in ("NOT_IMPLEMENTED", "IMPLEMENTED"):
                issues.append(f"missing ability implementation status for {name}/{key}")
            level=skill.get("level")
            if not isinstance(level,int) or not 1<=level<=50:
                issues.append(f"invalid skill level {level!r} for {name}/{key}")
        for key,skill in byskill.items():
            for prerequisite in skill.get("requires",[]):
                if prerequisite not in byskill:
                    issues.append(f"unresolved prerequisite {prerequisite} for {name}/{key}")
                elif isinstance(byskill[prerequisite].get("level"),int) and isinstance(skill.get("level"),int) and byskill[prerequisite]["level"]>=skill["level"]:
                    issues.append(f"prerequisite level ordering invalid for {name}/{key}")
        visited=set()
        def walk(key, stack):
            if key in stack:
                issues.append(f"skill cycle in {name}: {key}")
                return
            if key in visited:
                return
            visited.add(key)
            for req in byskill[key].get("requires",[]):
                if req in byskill:
                    walk(req,stack|{key})
        for key in byskill:
            walk(key,set())
    if manifest.get("status")=="PLAYABLE" and any(skill.get("implementation")!="IMPLEMENTED" for tree in trees for skill in tree.get("skills",[])):
        issues.append("playable manifest contains unimplemented skills")
    if manifest.get("status")=="PLAYABLE" and set(names)-set(byname):
        issues.append("playable manifest is missing complete class skill trees")
    return issues

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("manifest",type=Path)
    args=p.parse_args()
    try:
        manifest=json.loads(args.manifest.read_text(encoding="utf-8"))
        issues=validate(manifest)
    except (OSError,ValueError,TypeError) as e:
        print(f"INVALID: {e}",file=sys.stderr)
        return 2
    if issues:
        for issue in issues:
            print(f"ERROR: {issue}")
        return 1
    print(f"Manifest structurally valid ({len(manifest.get('candidates',[]))} candidates; "
          f"{len(manifest.get('skill_trees',[]))} trees). "
          "Does not prove spells or skills are implemented.")
    return 0

if __name__=="__main__":
    sys.exit(main())
