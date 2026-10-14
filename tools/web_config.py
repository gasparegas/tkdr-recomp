#!/usr/bin/env python3
"""Write the page's config.json from environment variables (deploy step).

usage: web_config.py DIR

  PICK_FILES  true (default): players bring their own files
              false: the game is loaded from GAME_ZIP (your own private server only)
  GAME_ZIP    URL of the zip (ISO/CSO + EBOOT), relative to the page or absolute

On a public host (e.g. Render) leave PICK_FILES unset or true.
"""
import json
import os
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
pick = os.environ.get("PICK_FILES", "true").strip().lower() not in ("false", "0", "no", "off")
zip_url = os.environ.get("GAME_ZIP", "").strip()
if not pick and not zip_url:
    sys.exit("PICK_FILES=false needs GAME_ZIP (the address of your zip)")
config = {"pickFiles": pick, "gameZip": "" if pick else zip_url}
path = os.path.join(sys.argv[1], "config.json")
with open(path, "w") as f:
    json.dump(config, f, indent=2)
    f.write("\n")
print(f"{path}: {'players pick their files' if pick else 'game loaded from ' + zip_url}")
