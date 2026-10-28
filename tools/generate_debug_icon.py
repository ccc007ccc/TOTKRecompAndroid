"""Create the Debug resource overlay from this project's existing artwork (requires Pillow)."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / "assets/artwork/triforce-icon-v2.png"
output = ROOT / "android/botwrecompandroid/app/src/debug/res/mipmap-xxxhdpi/ic_launcher.png"

with Image.open(source) as original:
    icon = original.convert("RGB").resize((512, 512), Image.Resampling.LANCZOS)
hue, saturation, value = icon.convert("HSV").split()
hue = hue.point(lambda pixel: (pixel + 114) % 256)
icon = Image.merge("HSV", (hue, saturation, value)).convert("RGB")
draw = ImageDraw.Draw(icon)
draw.rounded_rectangle((140, 365, 372, 451), radius=22, fill="#123A70", outline="#A0DFFF", width=4)
font = ImageFont.truetype(str(ROOT / "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf"), 72)
draw.text((256, 411), "DBG", font=font, fill="white", anchor="mm")
output.parent.mkdir(parents=True, exist_ok=True)
icon.save(output, optimize=True)
print(output)
