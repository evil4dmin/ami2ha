"""Write classic AmigaOS .info icon files.

There is no icon editor on this side of the wire, so the format is written
out directly. It is the original Commodore layout, understood by every
AmigaOS from 1.x onwards:

    DiskObject      78 bytes
    DrawerData      56 bytes   (drawers and disks only)
    Image           20 bytes + planar data   (the normal image)
    Image           20 bytes + planar data   (the selected image, optional)
    DefaultTool     LONG length + NUL-terminated string
    ToolTypes       LONG (n+1)*4, then each: LONG length + NUL-terminated
    ToolWindow      LONG length + string

Images are planar: all rows of plane 0, then all rows of plane 1, and so on,
each row padded out to a whole number of 16-bit words.

Only four colours are used, which are the Workbench palette every OS3
machine boots with, so the icon looks native without shipping a palette:

    0 grey (background)   1 black   2 white   3 blue

    python3 mkinfo.py <outdir>
"""

import struct
import sys

MAGIC = 0xE310

WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4

GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3
DEPTH = 2                      # four colours

W, H = 48, 48


# ------------------------------------------------------------------ #
# the artwork                                                        #
# ------------------------------------------------------------------ #

def blank():
    return [[GREY] * W for _ in range(H)]


def px(g, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c


def hline(g, x0, x1, y, c):
    for x in range(min(x0, x1), max(x0, x1) + 1):
        px(g, x, y, c)


def vline(g, x, y0, y1, c):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        px(g, x, y, c)


def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        hline(g, x0, x1, y, c)


def house(g, fill=GREY, edge=BLACK):
    """The silhouette the Open Home Foundation permitted us to keep."""
    apex_y, eaves_y, base_y = 5, 22, 42
    cx = W // 2
    for y in range(apex_y, eaves_y + 1):
        half = (y - apex_y) + 2
        hline(g, cx - half, cx + half - 1, y, fill)
    rect(g, 6, eaves_y + 1, W - 7, base_y, fill)
    for i in range(3):
        for x in range(6, 6 + 3 - i):
            px(g, x, base_y - i, GREY)
            px(g, W - 1 - x, base_y - i, GREY)
    for y in range(apex_y, eaves_y + 1):
        half = (y - apex_y) + 2
        px(g, cx - half, y, edge)
        px(g, cx + half - 1, y, edge)
    vline(g, 6, eaves_y + 1, base_y - 3, edge)
    vline(g, W - 7, eaves_y + 1, base_y - 3, edge)
    hline(g, 9, W - 10, base_y, edge)


FONT = {
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'H': ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    '2': ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    'I': ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
}


def text(g, s, x, y, c):
    for ch in s:
        for dy, row in enumerate(FONT[ch]):
            for dx, bit in enumerate(row):
                if bit == "1":
                    px(g, x + dx, y + dy, c)
        x += 6


def window(g, label, bar=BLUE, body=WHITE, edge=BLACK):
    """A Workbench window inside the house -- what the program actually is.

    The title bar is the Workbench blue every OS3 desktop already uses. The
    house itself stays grey, so no blue touches the shape derived from the
    Home Assistant logo.
    """
    x0, y0, x1, y1 = 13, 25, 34, 40
    rect(g, x0, y0, x1, y1, body)
    rect(g, x0, y0, x1, y0 + 4, bar)
    for x in range(x0, x1 + 1):
        px(g, x, y0, edge)
        px(g, x, y1, edge)
    for y in range(y0, y1 + 1):
        px(g, x0, y, edge)
        px(g, x1, y, edge)
    hline(g, x0, x1, y0 + 5, edge)
    px(g, x0 + 2, y0 + 2, WHITE)      # close gadget
    px(g, x1 - 2, y0 + 2, WHITE)      # depth gadget
    w = len(label) * 6 - 1
    text(g, label, x0 + 1 + (21 - w) // 2, y0 + 7, edge)


def art(label="A2H"):
    g = blank()
    house(g)
    window(g, label)
    return g


# ------------------------------------------------------------------ #
# the file format                                                    #
# ------------------------------------------------------------------ #

def planar(grid):
    """Chunky rows of colour indices -> planar words, plane by plane."""
    words = (W + 15) // 16
    out = bytearray()
    for plane in range(DEPTH):
        for row in grid:
            for wi in range(words):
                bits = 0
                for b in range(16):
                    x = wi * 16 + b
                    v = row[x] if x < W else 0
                    if (v >> plane) & 1:
                        bits |= 1 << (15 - b)
                out += struct.pack('>H', bits)
    return bytes(out)


def image_chunk(grid):
    """struct Image, then its data."""
    data = planar(grid)
    hdr = struct.pack('>hhhhh', 0, 0, W, H, DEPTH)      # Left,Top,W,H,Depth
    hdr += struct.pack('>I', 1)                          # ImageData (non-NULL)
    hdr += struct.pack('>BB', (1 << DEPTH) - 1, 0)       # PlanePick, PlaneOnOff
    hdr += struct.pack('>I', 0)                          # NextImage
    return hdr + data


def gadget(has_select):
    g = struct.pack('>I', 0)                    # NextGadget
    g += struct.pack('>hhhh', 0, 0, W, H)       # Left, Top, Width, Height
    g += struct.pack('>hhh', 4, 3, 1)           # Flags GADGIMAGE|GADGHBOX,
    #                                             Activation RELVERIFY|GADGIMMEDIATE,
    #                                             GadgetType BOOLGADGET
    g += struct.pack('>I', 1)                   # GadgetRender (non-NULL)
    g += struct.pack('>I', 1 if has_select else 0)
    g += struct.pack('>I', 0)                   # GadgetText
    g += struct.pack('>i', 0)                   # MutualExclude
    g += struct.pack('>I', 0)                   # SpecialInfo
    g += struct.pack('>H', 0)                   # GadgetID
    g += struct.pack('>I', 0)                   # UserData
    assert len(g) == 44, len(g)
    return g


def astring(s):
    b = s.encode('latin-1') + b'\0'
    return struct.pack('>I', len(b)) + b


def drawer_data():
    """A window for the drawer to open. Values a stock Workbench is happy with."""
    nw = struct.pack('>hhhh', 50, 40, 400, 150)   # Left, Top, Width, Height
    nw += struct.pack('>BB', 255, 255)            # DetailPen, BlockPen
    nw += struct.pack('>I', 0)                    # IDCMPFlags
    nw += struct.pack('>I', 0x2000ff4f)           # Flags: the usual drawer set
    nw += struct.pack('>I', 0) * 5                # Gadget, CheckMark, Title,
    #                                               Screen, BitMap
    nw += struct.pack('>hh', 90, 40)              # MinWidth, MinHeight
    nw += struct.pack('>HH', 0xffff, 0xffff)      # MaxWidth, MaxHeight
    nw += struct.pack('>H', 1)                    # Type: WBENCHSCREEN
    assert len(nw) == 48, len(nw)
    return nw + struct.pack('>ii', 0, 0)          # CurrentX, CurrentY


def diskobject(icon_type, grid, default_tool=None, tooltypes=None,
               stack=8192):
    is_drawer = icon_type in (WBDISK, WBDRAWER)

    do = struct.pack('>HH', MAGIC, 1)
    do += gadget(False)
    do += struct.pack('>BB', icon_type, 0)
    do += struct.pack('>I', 1 if default_tool else 0)
    do += struct.pack('>I', 1 if tooltypes else 0)
    # NO_ICON_POSITION is 0x80000000, not -1. Get this wrong and Workbench
    # takes it literally, placing every icon at (-1,-1) -- one pile with only
    # the topmost one visible.
    do += struct.pack('>II', 0x80000000, 0x80000000)
    do += struct.pack('>I', 1 if is_drawer else 0)   # DrawerData
    do += struct.pack('>I', 0)                       # ToolWindow
    do += struct.pack('>i', stack)
    assert len(do) == 78, len(do)

    out = bytearray(do)
    if is_drawer:
        out += drawer_data()
    out += image_chunk(grid)
    if default_tool:
        out += astring(default_tool)
    if tooltypes:
        out += struct.pack('>I', (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            out += astring(t)
    return bytes(out)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."

    # The program itself. WRITEICON keeps an existing icon and only rewrites
    # its tool types, so shipping this means the artwork survives.
    open(out + "/ami2ha_tool.info", "wb").write(
        diskobject(WBTOOL, art("A2H"),
                   tooltypes=["(CONFIG=Work:ami2ha/ami2ha.cfg)"],
                   stack=32768))

    # The installer script. A project icon whose default tool is Installer --
    # which does not live on the command path, so it has to be spelled out.
    open(out + "/Install_project.info", "wb").write(
        diskobject(WBPROJECT, art("I"),
                   default_tool="SYS:System/Installer"))

    # The drawer the archive unpacks into, so it is visible on Workbench.
    open(out + "/ami2ha_drawer.info", "wb").write(
        diskobject(WBDRAWER, art("A2H")))

    for n in ("ami2ha_tool", "Install_project", "ami2ha_drawer"):
        import os
        print("%-22s %d bytes" % (n + ".info",
                                  os.path.getsize(out + "/" + n + ".info")))


if __name__ == "__main__":
    main()
