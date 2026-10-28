#!/usr/bin/env python3
"""Renders the README screenshots into docs/ without touching the real screens.

    QT_QPA_PLATFORM=offscreen python3 tools/screenshots.py

Uses the current layout (trueedged --print), so the pictures show this desk.
"""
import copy
import importlib.machinery
import importlib.util
import os
import sys

from PyQt5 import QtCore, QtGui, QtWidgets

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
DOCS = os.path.join(ROOT, "docs")


def load_config_module():
    loader = importlib.machinery.SourceFileLoader("trueedge_config", os.path.join(ROOT, "trueedge-config"))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(module)
    return module


def screen_image(cfg, window, mon):
    """One monitor's pixels as Qt would show them: dark desktop plus its four rulers."""
    g = cfg.screen_geometry(mon)
    img = QtGui.QImage(g.width(), g.height(), QtGui.QImage.Format_RGB32)
    img.fill(QtGui.QColor("#2b3036"))
    p = QtGui.QPainter(img)
    p.setPen(QtGui.QColor("#8a939c"))
    font = QtGui.QFont()
    font.setPixelSize(64)
    p.setFont(font)
    p.drawText(img.rect(), QtCore.Qt.AlignCenter, mon["name"])
    for strip in window.rulers:
        if strip.item.mon is mon:
            p.drawPixmap(strip.geometry().topLeft() - g.topLeft(), strip.grab())
    p.end()
    return img


def desk(cfg, window, real, scale, crop_mm=None):
    """Places every screen at its real physical position (real: name -> mm dict),
    scale px per mm, with a bezel around it; optionally crops to (x, y, w, h) mm."""
    bezel, margin = 8, 20
    lo_x = min(m["x"] for m in real.values()) - margin
    lo_y = min(m["y"] for m in real.values()) - margin
    hi_x = max(m["x"] + m["w"] for m in real.values()) + margin
    hi_y = max(m["y"] + m["h"] for m in real.values()) + margin
    canvas = QtGui.QImage(round((hi_x - lo_x) * scale), round((hi_y - lo_y) * scale), QtGui.QImage.Format_RGB32)
    canvas.fill(QtGui.QColor("#d9d4cc"))  # the desk/wall
    p = QtGui.QPainter(canvas)
    p.setRenderHint(QtGui.QPainter.SmoothPixmapTransform)
    rect = lambda m, grow=0: QtCore.QRectF((m["x"] - lo_x - grow) * scale, (m["y"] - lo_y - grow) * scale,
                                           (m["w"] + 2 * grow) * scale, (m["h"] + 2 * grow) * scale)
    for m in real.values():
        p.fillRect(rect(m, bezel), QtGui.QColor("#111"))
    for item in window.items:
        p.drawImage(rect(real[item.mon["name"]]), screen_image(cfg, window, item.mon))
    p.end()
    if crop_mm:
        x, y, w, h = crop_mm
        canvas = canvas.copy(round((x - lo_x) * scale), round((y - lo_y) * scale), round(w * scale), round(h * scale))
    return canvas


def main():
    cfg = load_config_module()
    app = QtWidgets.QApplication(sys.argv)
    window = cfg.Window()
    window.resize(1100, 560)
    window.show()
    window.items[-1].setSelected(True)
    window.ruler_toggle.setChecked(True)
    for _ in range(3):
        app.processEvents()
    window.fit()
    os.makedirs(DOCS, exist_ok=True)
    window.grab().save(os.path.join(DOCS, "editor.png"))

    # where the screens really are: the layout plus a bezel gap between neighbours
    real = {item.mon["name"]: copy.deepcopy(item.mon) for item in window.items}
    for m in real.values():
        m["x"] += 16 * sum(o["x"] + o["w"] <= m["x"] + 1 for o in real.values() if o is not m)
    desk(cfg, window, real, 1.6).save(os.path.join(DOCS, "rulers.png"))

    # close-ups of the shared edge: layout matching the desk vs. a layout 8 mm off
    a, b = sorted(window.items, key=lambda i: i.mon["x"])[:2]
    edge = (real[a.mon["name"]]["x"] + real[a.mon["name"]]["w"] + real[b.mon["name"]]["x"]) / 2
    top = max(a.mon["y"], b.mon["y"])
    crop = (edge - 45, top + 40, 90, 110)
    desk(cfg, window, real, 4, crop).save(os.path.join(DOCS, "bezel-aligned.png"))
    b.mon["y"] += 8
    desk(cfg, window, real, 4, crop).save(os.path.join(DOCS, "bezel-misaligned.png"))
    window.show_rulers(False)


if __name__ == "__main__":
    main()
