#!/usr/bin/env python3
"""Parametric two-part enclosure for the Attendance Management System PCB.

Writes binary STL files to stl/ next to this script:
  base.stl     tray: battery bay under the board, board screwed down through the floor
  lid.stl      cover, already flipped top-down for printing
  button.stl   plunger for the POWER switch (SW3)
  assembly.stl base + lid + button in place, for a visual fit check only

Needs:  pip install manifold3d numpy

Hardware: 4x M3x6 countersunk (floor into the standoffs), 4x M3x16 countersunk
(lid into the same standoffs from the top). The bay under the board takes a
LiPo pouch up to about 45 x 70 x 7 mm.

Coordinates: x = KiCad x - 100, y = 145 - KiCad y (board spans 0..60 x 0..88.5,
USB-C edge at y = 0), z = 0 at the underside of the base. All values in mm,
taken from PCB/Attendance Management System.kicad_pcb (2026-10-06).
"""
import os
import struct

import numpy as np
from manifold3d import CrossSection, Manifold, set_circular_segments

set_circular_segments(96)

# ---------------------------------------------------------------- board facts
BOARD_W, BOARD_L, BOARD_R = 60.0, 88.5, 2.0      # Edge.Cuts outline, corner radius
BOARD_T = 1.6
STANDOFF_BELOW = 9.0      # Wuerth 9775106960 body below the board's underside
FLANGE_H = 1.1            # standoff flange above the board's top
MOUNT_HOLES = [(4.25, 84.25), (55.75, 84.25), (4.25, 38.25), (55.75, 4.25)]

USB_X = 30.04             # J1 (GCT USB4110) centre; shell is 8.94 x 3.26, front flush with edge
USB_SHELL_H = 3.26
SW_POWER = (4.5, 6.0)     # SW3, PTS636 2.5 mm tall
SW_BOOT0 = (12.0, 6.0)    # SW2
SW_RESET = (19.5, 6.0)    # SW1
SW_TOP = 2.58             # switch actuator height above board
STATUS_LEDS = [(12.5, 15.74), (15.0, 15.74)]              # D6 green, D5 red (PA2/PA3)
CHARGE_LEDS = [(39.0, 2.5), (41.5, 2.5), (44.0, 2.5)]     # charger STAT1/STAT2/PG
LED_TOP = 1.2
BATT_JST = (56.2, 12.65)  # J2 (JST-XA vertical) body centre; mated plug is 11.3 mm tall
ANTENNA = (29.5, 68.5, 40.0, 30.0)  # AE3 centre and loop size

# ---------------------------------------------------------------- case parameters
GAP = 0.4                 # board edge to wall
WALL = 2.2
FLOOR = 2.2
TOP = 2.0
HEADROOM = 12.0           # inside height above the board: JST-XA plug 11.3 mm, headers 8.6 mm
JST_POCKET = 1.2          # extra ceiling relief above the battery plug for the wire bend
LIP_T, LIP_H, LIP_CLR = 1.0, 1.6, 0.15   # base lip that locates the lid
SCREW_D, CSK_D, CSK_H = 3.4, 6.6, 1.6    # M3 countersunk
USB_CUT_W, USB_CUT_H, USB_CUT_R = 12.4, 7.0, 1.5   # room for the cable overmould

Z_BOARD_BOT = FLOOR + STANDOFF_BELOW
Z_BOARD_TOP = Z_BOARD_BOT + BOARD_T      # base/lid split plane
Z_CEIL = Z_BOARD_TOP + HEADROOM
Z_TOP = Z_CEIL + TOP


def rrect(w, l, r, x0=0.0, y0=0.0):
    """Rounded rectangle with its lower-left corner at (x0, y0)."""
    return (CrossSection.square((w - 2 * r, l - 2 * r))
            .translate((x0 + r, y0 + r)).offset(r))


def board_outline(grow):
    return rrect(BOARD_W, BOARD_L, BOARD_R).offset(grow)


def slab(cs, z0, z1):
    return Manifold.extrude(cs, z1 - z0).translate((0, 0, z0))


def cyl(x, y, z0, z1, d, d_top=None):
    r = d / 2
    rt = r if d_top is None else d_top / 2
    return Manifold.cylinder(z1 - z0, r, rt).translate((x, y, z0))


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))


def usb_cut():
    """Rounded-rectangle opening through the front wall, centred on the receptacle."""
    zc = Z_BOARD_TOP + USB_SHELL_H / 2
    r = USB_CUT_R
    cs = (CrossSection.square((USB_CUT_W - 2 * r, USB_CUT_H - 2 * r), center=True)
          .offset(r))
    # cross-section lies in XY; turn it to stand in the XZ plane, extrude along -y
    m = Manifold.extrude(cs, WALL + GAP + 1.5).rotate((90, 0, 0))
    return m.translate((USB_X, 1.0, zc))


def battery_wire_groove():
    """Channel in the right-hand wall so the JST lead can drop beside the board."""
    y = BATT_JST[1]
    return box(BOARD_W + GAP - 0.01, y - 3.5, FLOOR, BOARD_W + GAP + 1.2, y + 3.5, Z_CEIL)


def light_cells(leds, pitch_axis_w, wall=0.6):
    """Walled light pipes from the lid down to just above a row of LEDs (along x)."""
    xs = [p[0] for p in leds]
    y = leds[0][1]
    z0, z1 = Z_BOARD_TOP + LED_TOP + 0.8, Z_CEIL + 0.01
    half = pitch_axis_w / 2
    outer = box(min(xs) - 1.25 - wall, y - half - wall, z0, max(xs) + 1.25 + wall, y + half + wall, z1)
    cells = Manifold()
    for x in xs:
        cells += box(x - 1.25 + wall / 2, y - half, z0 - 0.1, x + 1.25 - wall / 2, y + half, z1 + 0.1)
    holes = Manifold()
    for x in xs:
        holes += cyl(x, y, Z_CEIL - 0.1, Z_TOP + 0.1, 1.8)
    return outer - cells, holes


def make_base():
    outer = board_outline(GAP + WALL)
    inner = board_outline(GAP)
    base = slab(outer, 0, Z_BOARD_TOP) - slab(inner, FLOOR, Z_BOARD_TOP + 1)
    lip = slab(inner.offset(LIP_T), Z_BOARD_TOP - 0.01, Z_BOARD_TOP + LIP_H) - slab(inner, Z_BOARD_TOP - 1, Z_BOARD_TOP + LIP_H + 1)
    base += lip
    for x, y in MOUNT_HOLES:
        base += cyl(x, y, FLOOR - 0.01, FLOOR + 0.8, 6.6) - cyl(x, y, FLOOR - 1, FLOOR + 1, 4.6)
        base -= cyl(x, y, -0.1, FLOOR + 1, SCREW_D)
        base -= cyl(x, y, -0.01, CSK_H, CSK_D, SCREW_D)
    base -= usb_cut()
    base -= battery_wire_groove()
    return base


def make_lid():
    outer = board_outline(GAP + WALL)
    inner = board_outline(GAP)
    lid = slab(outer, Z_BOARD_TOP, Z_TOP) - slab(inner, Z_BOARD_TOP - 1, Z_CEIL)
    lid -= slab(inner.offset(LIP_T + LIP_CLR), Z_BOARD_TOP - 1, Z_BOARD_TOP + LIP_H + 0.2)

    # screw bosses: land 0.3 mm above the standoff flanges so the lid seats on the base rim
    for x, y in MOUNT_HOLES:
        lid += cyl(x, y, Z_BOARD_TOP + FLANGE_H + 0.3, Z_CEIL + 0.01, 6.4)
        lid -= cyl(x, y, Z_BOARD_TOP, Z_TOP + 1, SCREW_D)
        lid -= cyl(x, y, Z_TOP - CSK_H, Z_TOP + 0.01, SCREW_D, CSK_D)

    # power button: hole for the plunger cap, sleeve that keeps the plunger upright
    px, py = SW_POWER
    lid += cyl(px, py, Z_BOARD_TOP + 4.0, Z_CEIL + 0.01, 9.9) - cyl(px, py, Z_BOARD_TOP, Z_CEIL + 0.02, 7.9)
    lid -= cyl(px, py, Z_CEIL - 0.1, Z_TOP + 0.1, 5.0)

    # RESET / BOOT0: paper-clip holes with guide tubes down to the switches
    for x, y in (SW_RESET, SW_BOOT0):
        lid += cyl(x, y, Z_BOARD_TOP + SW_TOP + 1.0, Z_CEIL + 0.01, 3.6)
        lid -= cyl(x, y, Z_BOARD_TOP, Z_TOP + 0.1, 1.5)

    for leds in (STATUS_LEDS, CHARGE_LEDS):
        walls, holes = light_cells(leds, 2.0)
        lid += walls
        lid -= holes

    # wire-bend relief above the battery plug
    jx, jy = BATT_JST
    lid -= box(jx - 5.0, jy - 5.5, Z_CEIL - 0.1, BOARD_W + GAP, jy + 5.5, Z_CEIL + JST_POCKET)

    lid -= usb_cut()
    lid -= battery_wire_groove()

    # tap target over the NFC loop: 0.5 mm deep outline on the top face
    ax, ay, aw, al = ANTENNA
    ring = (rrect(aw + 4, al + 4, 4, ax - aw / 2 - 2, ay - al / 2 - 2)
            - rrect(aw + 1.6, al + 1.6, 2.8, ax - aw / 2 - 0.8, ay - al / 2 - 0.8))
    lid -= slab(ring, Z_TOP - 0.5, Z_TOP + 0.1)
    return lid


def make_button():
    """POWER plunger, modelled in its installed position (resting on the switch)."""
    px, py = SW_POWER
    z_stem = Z_BOARD_TOP + SW_TOP
    z_flange = Z_CEIL - 0.3 - 1.2
    z_cap_top = Z_TOP + 1.0 - 0.3
    return (cyl(px, py, z_stem, z_flange + 0.01, 3.0)
            + cyl(px, py, z_flange, z_flange + 1.2, 7.4)
            + cyl(px, py, z_flange + 1.19, z_cap_top, 4.5))


def write_stl(path, m):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties, dtype=np.float32)[:, :3]
    t = np.asarray(mesh.tri_verts, dtype=np.int64)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(t), dtype=np.dtype([('n', '<3f4'), ('v', '<9f4'), ('a', '<u2')]))
    rec['n'] = n
    rec['v'] = tri.reshape(-1, 9)
    with open(path, 'wb') as f:
        f.write(b'Attendance Management System enclosure'.ljust(80, b' '))
        f.write(struct.pack('<I', len(t)))
        f.write(rec.tobytes())


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'stl')
    os.makedirs(out, exist_ok=True)
    base, lid, button = make_base(), make_lid(), make_button()
    for name, m in (('base', base), ('lid', lid), ('button', button)):
        assert m.status().name == 'NoError' and m.genus() >= 0, name
    write_stl(os.path.join(out, 'assembly.stl'), base + lid + button)

    # print orientations: everything sits on z = 0
    def to_bed(m):
        return m.translate((0, 0, -m.bounding_box()[2]))
    write_stl(os.path.join(out, 'base.stl'), base)
    write_stl(os.path.join(out, 'lid.stl'), to_bed(lid.rotate((180, 0, 0))))
    write_stl(os.path.join(out, 'button.stl'), to_bed(button.rotate((180, 0, 0))))

    bb = base.bounding_box()
    print(f'outside: {bb[3] - bb[0]:.1f} x {bb[4] - bb[1]:.1f} x {Z_TOP:.1f} mm')
    print(f'board top at z = {Z_BOARD_TOP:.1f}, ceiling at {Z_CEIL:.1f}')
    print(f'top screws: M3 countersunk, {Z_TOP - (Z_BOARD_TOP + FLANGE_H):.1f} mm to the standoff top')
    print(f'battery bay: {STANDOFF_BELOW:.1f} mm tall under the board')
    for name, m in (('base', base), ('lid', lid), ('button', button)):
        print(f'{name}: {m.volume() / 1000:.1f} cm^3, {m.num_tri()} triangles')


if __name__ == '__main__':
    main()
