import os
import json
import datetime
import PIL.Image as Image
import PIL.ImageOps as ImageOps
import PIL.ImageEnhance as ImageEnhance
import PIL.ImageFile as ImageFile

# --- SETTINGS ---
BASE_PATH = "/config/www/eink_frame"
DB_FILE = f"{BASE_PATH}/eink_db.json"
OUTPUT_BIN = f"{BASE_PATH}/output.bin"
OUTPUT_PREVIEW = f"{BASE_PATH}/preview_dithered.png"
POOL_PATH = f"{BASE_PATH}/pool_all"

ImageFile.LOAD_TRUNCATED_IMAGES = True

@pyscript_compile
def load_db(db_path):
    if not os.path.exists(db_path):
        return {"current_image": "", "images": {}}
    with open(db_path, 'r') as f:
        return json.load(f)

@pyscript_compile
def save_db_json(path, data):
    with open(path, 'w') as f:
        json.dump(data, f, indent=2)

@pyscript_compile
def get_next_selected_image(path, db):
    if not os.path.exists(path):
        return None, "Folder pool_all missing"
        
    folder_files = [f for f in os.listdir(path) if f.lower().endswith(('.jpg', '.png', '.jpeg'))]
    selected_files = [f for f, data in db["images"].items() if data.get("selected") == True and f in folder_files]
    
    if not selected_files:
        return None, "No images selected in loop"

    current = db.get("current_image", "")
    try:
        next_idx = (selected_files.index(current) + 1) % len(selected_files)
    except ValueError:
        next_idx = 0
    
    return selected_files[next_idx], None

@pyscript_compile
def _color_distance_sq(r1, g1, b1, r2, g2, b2):
    """Squared Euclidean distance for palette matching."""
    dr, dg, db = r1 - r2, g1 - g2, b1 - b2
    return dr*dr + dg*dg + db*db

@pyscript_compile
def _nearest_index(r, g, b, palette_rgb):
    """Finds closest color in the measured physical palette."""
    best_d = float("inf")
    best_i = 0
    for i, (pr, pg, pb) in enumerate(palette_rgb):
        d = _color_distance_sq(r, g, b, pr, pg, pb)
        if d < best_d:
            best_d = d
            best_i = i
    return best_i

@pyscript_compile
def _dither_floyd_steinberg(img_rgb, w, h, palette_rgb):
    """Floyd-Steinberg dithering using measured hardware colors."""
    buf = [[list(img_rgb.getpixel((x, y))) for x in range(w)] for y in range(h)]
    indices = []

    for y in range(h):
        for x in range(w):
            r = max(0, min(255, int(buf[y][x][0])))
            g = max(0, min(255, int(buf[y][x][1])))
            b = max(0, min(255, int(buf[y][x][2])))

            idx = _nearest_index(r, g, b, palette_rgb)
            indices.append(idx)

            pr, pg, pb = palette_rgb[idx]
            er, eg, eb = r - pr, g - pg, b - pb

            if x + 1 < w:
                buf[y][x+1][0] += er * 7 / 16
                buf[y][x+1][1] += eg * 7 / 16
                buf[y][x+1][2] += eb * 7 / 16
            if y + 1 < h:
                if x - 1 >= 0:
                    buf[y+1][x-1][0] += er * 3 / 16
                    buf[y+1][x-1][1] += eg * 3 / 16
                    buf[y+1][x-1][2] += eb * 3 / 16
                buf[y+1][x][0] += er * 5 / 16
                buf[y+1][x][1] += eg * 5 / 16
                buf[y+1][x][2] += eb * 5 / 16
                if x + 1 < w:
                    buf[y+1][x+1][0] += er * 1 / 16
                    buf[y+1][x+1][1] += eg * 1 / 16
                    buf[y+1][x+1][2] += eb * 1 / 16
    return indices

@pyscript_compile
def _rotate_indices_90cw(indices, w, h):
    """Rotates portrait indices to landscape for hardware buffer."""
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            out[x * h + (h - 1 - y)] = indices[y * w + x]
    return out

@pyscript_compile
def process_and_save(file_in, preview_out, bin_out):
    # 1. Image Prep
    img = Image.open(file_in)
    img = ImageOps.exif_transpose(img).convert("RGB")
    target_w, target_h = 480, 800
    
    if img.width > img.height:
        img = img.rotate(90, expand=True)

    resized = ImageOps.pad(img, size=(target_w, target_h), color=(255, 255, 255))
    
    # Pre-dither enhancements
    enhanced = ImageEnhance.Contrast(resized).enhance(1.2)
    enhanced = ImageEnhance.Brightness(enhanced).enhance(1.05)

    # 2. Phase 1: Dither with MEASURED colors (Proven Mapping Order)
    # 0:Black, 1:White, 2:Yellow, 3:Red, 4:Green, 5:Blue
    measured = [
        ( 31,  31,  31), (228, 222, 214), (210, 185,  55), 
        (185,  40,  35), ( 48, 120,  55), ( 30,  60, 130)
    ]
    indices = _dither_floyd_steinberg(enhanced, target_w, target_h, measured)

    # 3. Phase 2: Remap to IDEAL colors (Proven RGB values)
    ideal_rgb = [
        (0, 0, 0), (255, 255, 255), (255, 243, 56), 
        (191, 0, 0), (0, 131, 0), (0, 0, 170)
    ]
    ideal_flat = []
    for rgb in ideal_rgb:
        ideal_flat.extend(rgb)
    ideal_flat.extend([0] * (768 - len(ideal_flat)))

    # 4. Save Preview
    pal_img = Image.new("P", (target_w, target_h))
    pal_img.putpalette(ideal_flat)
    pal_img.putdata(indices)
    pal_img.save(preview_out)
    
    # 5. Rotate & Binary Pack
    hw_indices = _rotate_indices_90cw(indices, target_w, target_h)
    with open(bin_out, "wb") as f:
        for i in range(0, len(hw_indices), 2):
            byte = (hw_indices[i] << 4) | (hw_indices[i+1] & 0x0F)
            f.write(bytes([byte]))

@service
def process_next_image():
    db = task.executor(load_db, DB_FILE)
    next_file, error = task.executor(get_next_selected_image, POOL_PATH, db)
    
    if error:
        log.error(f"E-Ink Error: {error}")
        return

    task.executor(process_and_save, f"{POOL_PATH}/{next_file}", OUTPUT_PREVIEW, OUTPUT_BIN)
    
    db["current_image"] = next_file
    db["images"][next_file]["last_shown"] = datetime.datetime.now().isoformat()
    task.executor(save_db_json, DB_FILE, db)
    log.info(f"SUCCESS: {next_file} is now active.")
