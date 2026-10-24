#!/usr/bin/env python3
import os
import sys
import json
import urllib.request
from concurrent.futures import ThreadPoolExecutor

INDEX_URL = "https://launchermeta.mojang.com/v1/packages/f6ad102bcaa53b1a58358f16e376d548d44933ec/1.8.json"
BASE_RES_URL = "https://resources.download.minecraft.net"

def main():
    dest_dir = sys.argv[1] if len(sys.argv) > 1 else "build/arch/rootfs/opt/minecraft/assets"
    indexes_dir = os.path.join(dest_dir, "indexes")
    objects_dir = os.path.join(dest_dir, "objects")
    
    os.makedirs(indexes_dir, exist_ok=True)
    os.makedirs(objects_dir, exist_ok=True)
    
    index_file = os.path.join(indexes_dir, "1.8.json")
    print(f"[*] Fetching asset index from {INDEX_URL}...")
    req = urllib.request.urlopen(INDEX_URL)
    index_bytes = req.read()
    with open(index_file, "wb") as f:
        f.write(index_bytes)
        
    index_data = json.loads(index_bytes.decode("utf-8"))
    objects = index_data["objects"]
    
    # Filter all sound files and sounds.json (plus icons/langs if present)
    target_objects = []
    for path, info in objects.items():
        # Include sounds, sounds.json, and essentials
        if path.startswith("minecraft/sounds") or path == "minecraft/sounds.json" or path.startswith("minecraft/lang"):
            h = info["hash"]
            target_objects.append((h, path))
            
    print(f"[*] Downloading {len(target_objects)} sound & asset files...")
    
    def download_one(item):
        h, path = item
        prefix = h[:2]
        h_dir = os.path.join(objects_dir, prefix)
        os.makedirs(h_dir, exist_ok=True)
        h_file = os.path.join(h_dir, h)
        if os.path.exists(h_file) and os.path.getsize(h_file) > 0:
            return
        url = f"{BASE_RES_URL}/{prefix}/{h}"
        try:
            urllib.request.urlretrieve(url, h_file)
        except Exception as e:
            print(f"[!] Failed to download {path} ({h}): {e}")

    with ThreadPoolExecutor(max_workers=16) as executor:
        list(executor.map(download_one, target_objects))
        
    print("[+] Finished downloading Minecraft sound assets!")

if __name__ == "__main__":
    main()
