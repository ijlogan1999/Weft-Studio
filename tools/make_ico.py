"""Packs the rendered icon PNGs into app/resources/icon/weft.ico (see tools/render_icon.js)."""
from pathlib import Path

from PIL import Image

DIR = Path(__file__).resolve().parent.parent / "app" / "resources" / "icon"
SIZES = [256, 64, 48, 32, 24, 16]

images = [Image.open(DIR / f"weft-icon-{s}.png").convert("RGBA") for s in SIZES]
images[0].save(DIR / "weft.ico", sizes=[(s, s) for s in SIZES], append_images=images[1:])
