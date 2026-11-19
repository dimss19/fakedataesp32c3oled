import json
from pathlib import Path

Import("env")

project_dir = Path(env["PROJECT_DIR"])
json_path = project_dir / "rfid_tags.json"
header_path = project_dir / "include" / "rfid_tags_embedded.h"

if not json_path.is_file():
    raise FileNotFoundError(f"File tidak ditemukan: {json_path}")

raw_text = json_path.read_text(encoding="utf-8")

try:
    parsed = json.loads(raw_text)
except json.JSONDecodeError as exc:
    raise ValueError(f"Format JSON tidak valid pada {json_path}: {exc}") from exc

device_id = "PORTAL-MLG-01"
batch_mode = False

if isinstance(parsed, list):
    tags = parsed
elif isinstance(parsed, dict):
    tags = parsed.get("tags")
    if isinstance(parsed.get("device_id"), str) and parsed["device_id"].strip():
        device_id = parsed["device_id"].strip()
    if isinstance(parsed.get("mode"), str) and parsed["mode"].strip().lower() == "batch":
        batch_mode = True
else:
    raise ValueError(f"{json_path} harus berupa JSON array EPC atau object dengan field 'tags'.")

if not isinstance(tags, list) or len(tags) == 0:
    raise ValueError(f"{json_path} harus memiliki minimal 1 EPC.")

clean_tags = []
for idx, tag in enumerate(tags):
    if not isinstance(tag, str) or not tag.strip():
        raise ValueError(f"tags[{idx}] pada {json_path} harus berupa string EPC yang tidak kosong.")
    clean_tags.append(tag.strip())

tag_lines = ",\n".join(f"    {json.dumps(t, ensure_ascii=True)}" for t in clean_tags)

header_content = (
    "#pragma once\n"
    "// Auto-generated from rfid_tags.json before build. Edit rfid_tags.json instead.\n"
    "#include <stddef.h>\n"
    "#include <pgmspace.h>\n\n"
    f"static const char RFID_DEFAULT_DEVICE_ID[] = {json.dumps(device_id, ensure_ascii=True)};\n"
    f"static const bool RFID_DEFAULT_BATCH_MODE = {'true' if batch_mode else 'false'};\n"
    f"static const size_t RFID_TAG_COUNT = {len(clean_tags)};\n"
    "static const char *const RFID_TAGS[RFID_TAG_COUNT] PROGMEM = {\n"
    f"{tag_lines}\n"
    "};\n"
)

header_path.parent.mkdir(parents=True, exist_ok=True)
if not header_path.is_file() or header_path.read_text(encoding="utf-8") != header_content:
    header_path.write_text(header_content, encoding="utf-8")
    print(f"[RFID JSON] Embedded {len(clean_tags)} tags from rfid_tags.json -> include/rfid_tags_embedded.h")
