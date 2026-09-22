#!/usr/bin/env bash
# Add a DNN (APN name) to every subscriber's default slice in the running Open5GS core.
#
# Why: srsRAN's add_users.py subscribes only "srsapn" and "ims". Phones whose APN profile carries
# another name (e.g. the Pixels' "OAI-SA" profile with APN "oai") get "DNN Not Supported OR Not
# Subscribed in the Slice" from the AMF. Adding the DNN to the subscription (same as the WebUI would)
# lets them keep their APN. The SMF/UPF pool in the srsRAN recipe is not DNN-specific.
#
#   scripts/run/core_add_dnn.sh <dnn> [<dnn> ...]        (idempotent; start_core.sh calls it with P5G_UE_DNNS)
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: core_add_dnn.sh <dnn> [...]" >&2; exit 1; }
for dnn in "$@"; do
  docker exec -i -e DNN="$dnn" p5g_open5gs python3 - <<'PY'
import copy, os, pymongo
dnn = os.environ["DNN"]
db = pymongo.MongoClient("mongodb://127.0.0.1:27017").open5gs
n_added = 0
for sub in db.subscribers.find():
    changed = False
    for sl in sub.get("slice", []):
        sessions = sl.get("session", [])
        if any(s.get("name") == dnn for s in sessions):
            continue
        base = next((s for s in sessions if s.get("name") == "srsapn"), sessions[0] if sessions else None)
        if base is None:
            continue
        new = copy.deepcopy(base)
        new["name"] = dnn          # same QoS/AMBR/static UE IP as the default data session
        sessions.append(new)
        sl["session"] = sessions
        changed = True
    if changed:
        db.subscribers.update_one({"_id": sub["_id"]}, {"$set": {"slice": sub["slice"]}})
        n_added += 1
print(f"[core_add_dnn] dnn={dnn}: added to {n_added} subscriber(s)")
PY
done
