from PIL import Image, ImageDraw

sizes = [16, 20, 24, 32]
images = []

for size in sizes:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    stroke = max(1, size // 8)
    left = max(1, size // 8)
    top = max(1, size // 8)
    right = size - left - 1
    bottom = size - top - 1
    draw.rounded_rectangle(
        (left, top, right, bottom),
        radius=max(1, size // 8),
        outline=(70, 70, 70, 255),
        width=stroke,
    )
    draw.line(
        (size // 4, size // 2 - size // 8, size // 2 - size // 8, size // 2),
        fill=(35, 110, 210, 255),
        width=stroke,
    )
    draw.line(
        (size // 2 - size // 8, size // 2, size // 4, size // 2 + size // 8),
        fill=(35, 110, 210, 255),
        width=stroke,
    )
    draw.line(
        (size // 2 + size // 10, size * 3 // 4, size * 3 // 4, size * 3 // 4),
        fill=(35, 110, 210, 255),
        width=stroke,
    )
    images.append(image)

images[-1].save(
    "PowerEditor/src/icons/light/toolbar/regular/terminal_off.ico",
    format="ICO",
    sizes=[(size, size) for size in sizes],
)
images[-1].save(
    "PowerEditor/src/icons/dark/toolbar/regular/terminal_off.ico",
    format="ICO",
    sizes=[(size, size) for size in sizes],
)
