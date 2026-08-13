"""ami2ha Workbench icon -- pixel art kept as code, so it can be redrawn.

The house silhouette is a derivative of the Home Assistant logo, used with
the Open Home Foundation's permission (13 Aug 2026) under their conditions:
the house shape may be kept, the circuit-tree motif inside it must be
replaced, and their signature blue #18BCF2 must not be used.

So the interior is a Workbench window instead, in the Workbench palette,
labelled with the project's own name rather than anyone's trademark. No
Amiga marks are used either -- those belong to C-A Acquisition Corp.

    python3 ami2ha-icon.py [outdir]
"""
"""Draw candidate ami2ha icons at real Amiga icon size, no dependencies."""
import zlib, struct

W, H = 48, 48                      # a normal-ish AmigaOS tool icon
SCALE = 10                         # for viewing

# Workbench-era palette. Deliberately nothing near HA's #18BCF2.
BLACK  = (0x00, 0x00, 0x00)
WHITE  = (0xFF, 0xFF, 0xFF)
GREY   = (0xAA, 0xAA, 0xAA)
DGREY  = (0x66, 0x66, 0x66)
ORANGE = (0xFF, 0x88, 0x00)
WBBLUE = (0x3B, 0x67, 0xA2)        # Workbench 3.x navy, not HA cyan
BG     = (0x95, 0x95, 0x95)        # Workbench grey backdrop

def blank():
    return [[BG for _ in range(W)] for _ in range(H)]

def px(g, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c

def hline(g, x0, x1, y, c):
    for x in range(min(x0,x1), max(x0,x1)+1): px(g, x, y, c)

def vline(g, x, y0, y1, c):
    for y in range(min(y0,y1), max(y0,y1)+1): px(g, x, y, c)

def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1+1): hline(g, x0, x1, y, c)

def house(g, fill, edge):
    """Rounded-pentagon house: pitched roof over a body with clipped corners.
    Same silhouette family as the HA mark, drawn in chunky pixels."""
    apex_y, eaves_y, base_y = 5, 22, 42
    cx = W // 2
    # roof: widen by one pixel per row
    for y in range(apex_y, eaves_y+1):
        half = (y - apex_y) + 2
        hline(g, cx-half, cx+half-1, y, fill)
    # body
    rect(g, 6, eaves_y+1, W-7, base_y, fill)
    # clipped bottom corners, so it reads as rounded at this size
    for i in range(3):
        for x in range(6, 6+3-i):
            px(g, x, base_y-i, BG)
            px(g, W-1-x, base_y-i, BG)
    # outline
    for y in range(apex_y, eaves_y+1):
        half = (y - apex_y) + 2
        px(g, cx-half, y, edge); px(g, cx+half-1, y, edge)
    vline(g, 6, eaves_y+1, base_y-3, edge)
    vline(g, W-7, eaves_y+1, base_y-3, edge)
    hline(g, 9, W-10, base_y, edge)

def motif_window(g, bar, body, edge):
    """A Workbench window inside the house: 'your house, on your Workbench'."""
    x0, y0, x1, y1 = 14, 26, 33, 39
    rect(g, x0, y0, x1, y1, body)
    rect(g, x0, y0, x1, y0+3, bar)          # title bar
    for x in range(x0, x1+1):
        px(g, x, y0, edge); px(g, x, y1, edge)
    for y in range(y0, y1+1):
        px(g, x0, y, edge); px(g, x1, y, edge)
    hline(g, x0, x1, y0+4, edge)            # under the title bar
    px(g, x0+2, y0+1, edge); px(g, x0+2, y0+2, edge)   # close gadget
    hline(g, x0+3, x1-6, y0+7, edge)        # two lines of "content"
    hline(g, x0+3, x1-3, y0+10, edge)

def motif_bars(g, c, edge):
    """Ascending signal bars -- clearly not HA's circuit tree."""
    base = 39
    for i, h in enumerate((4, 8, 12)):
        x = 16 + i*7
        rect(g, x, base-h, x+4, base, c)
        for y in range(base-h, base+1):
            px(g, x, y, edge); px(g, x+4, y, edge)
        hline(g, x, x+4, base-h, edge); hline(g, x, x+4, base, edge)

def write_png(path, grid, scale=1):
    h = len(grid); w = len(grid[0])
    raw = b''
    for row in grid:
        for _ in range(scale):
            line = b'\x00'
            for c in row:
                line += bytes(c) * scale
            raw += line
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w*scale, h*scale, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


FONT = {   # chunky Topaz-ish 5x7, enough for the label
 'A': ["01110","10001","10001","11111","10001","10001","10001"],
 'H': ["10001","10001","10001","11111","10001","10001","10001"],
 '2': ["01110","10001","00001","00010","00100","01000","11111"],
}

def text(g, s, x, y, c):
    for ch in s:
        for dy, r in enumerate(FONT[ch]):
            for dx, b in enumerate(r):
                if b == "1":
                    px(g, x+dx, y+dy, c)
        x += 6

def window(g, bar, body, edge, label):
    """A Workbench window inside the house: what the program actually is."""
    x0, y0, x1, y1 = 13, 25, 34, 40
    rect(g, x0, y0, x1, y1, body)
    rect(g, x0, y0, x1, y0+4, bar)
    for x in range(x0, x1+1):
        px(g, x, y0, edge); px(g, x, y1, edge)
    for y in range(y0, y1+1):
        px(g, x0, y, edge); px(g, x1, y, edge)
    hline(g, x0, x1, y0+5, edge)
    px(g, x0+2, y0+2, edge)        # close gadget
    px(g, x1-2, y0+2, edge)        # depth gadget
    w = len(label)*6 - 1
    text(g, label, x0 + 1 + (21 - w)//2, y0+7, edge)

if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    g = blank()
    house(g, GREY, BLACK)
    window(g, ORANGE, WHITE, BLACK, "A2H")
    write_png(out + "/ami2ha-icon.png", g, 1)
    write_png(out + "/ami2ha-icon-8x.png", g, 8)
    print("wrote ami2ha-icon.png (%dx%d) and an 8x preview" % (W, H))
