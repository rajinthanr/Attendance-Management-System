#!/usr/bin/env python3
"""
Attendance Logger companion app: the desktop window, in Qt (PySide6).

Every action calls attendance_app.py (the device, and the actions and exports),
attendance_db.py (the database) and report_pages.py (PDFs); the wording and
rules of what the window says (the suggested lecture name, the banners, the
device status, the battery, a lecture's list) are app_helpers.py's, which has no
GUI in it.

    Lectures   start a lecture by name, and for each lecture see who came:
               index number, name and the time they tapped; save the list as
               CSV or PDF; clear every record on this computer
    Students   register cards (tap one on the plugged-in device): index number
               and name
    Device     connect (bring its drive back), read it now, eject it, set its
               clock, clear the records in its memory (after copying every one
               of them to this computer), or erase them without saving when
               they cannot be read

Needs PySide6:  python -m pip install -r requirements.txt
(start.sh / start.bat use the .venv folder next to this file when it is there.)

    python attendance_qt.py                     normal use
    python attendance_qt.py --demo              try it with made-up data, no device
    python attendance_qt.py --data-dir D:\\Attendance    keep the database elsewhere
    python attendance_qt.py --device-dir FOLDER  use a folder as the device

Device scanning and anything written to the device run on a worker thread
(app_helpers.Worker); a timer hands the results to the window, so it never
freezes while Windows wakes up a slow drive.
"""
import argparse
import os
import queue
import re
import shutil
import signal
import sys
import tempfile

try:
    from PySide6.QtCore import QPoint, QRectF, QSize, Qt, QTimer
    from PySide6.QtGui import QAction, QColor, QFont, QIcon, QKeySequence, QPainter, QPalette, QPen, QPixmap, QShortcut
    from PySide6.QtWidgets import (
        QAbstractItemView, QApplication, QCheckBox, QComboBox, QDialog, QFileDialog, QFrame,
        QGraphicsDropShadowEffect,
        QGridLayout, QHBoxLayout, QHeaderView, QInputDialog, QLabel, QLineEdit, QListWidget, QListWidgetItem,
        QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QSizePolicy, QSplitter, QStackedWidget, QStyle,
        QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)
except ImportError:          # main() says how to install it
    QApplication = None

import attendance_app as A
import attendance_db as D
import app_helpers as H

APP_TITLE = H.APP_TITLE
POLL_MS = H.POLL_MS
P = H.PAL                      # the window's colours

# Button looks: the property "variant" picks one in the style sheet below.
VARIANTS = {
    "primary": ("indigo", "indigo_dark"), "success": ("green", "green_dark"), "danger": ("red", "red_dark"),
    "info": ("blue", "blue_dark"), "teal": ("teal", "teal_dark"), "warning": ("orange", "orange_dark"),
    "purple": ("purple", "purple_dark"),
}


def style_sheet():
    """The whole window's look, in one place."""
    css = """
    QWidget { color: %(text)s; font-size: 10pt; }
    QMainWindow, QWidget#Page, QDialog { background: %(bg)s; }
    QFrame#Header { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #283593, stop:0.6 #3F51B5,
                    stop:1 #5C6BC0); }
    QLabel#AppTitle { color: white; font-size: 18pt; font-weight: 700; }
    QLabel#AppSub { color: #C5CAE9; font-size: 9pt; }
    QLabel#Chip { border-radius: 14px; padding: 6px 14px; font-weight: 600; }
    QLabel#Chip[kind="ok"] { background: #E8F5E9; color: #1B5E20; }
    QLabel#Chip[kind="wait"] { background: #FFF3E0; color: #E65100; }
    QLabel#Chip[kind="off"] { background: #ECEFF1; color: #37474F; }
    QLabel#Chip[kind="warn"] { background: #FFF3E0; color: #E65100; }
    QLabel#Chip[kind="bad"] { background: #FDECEA; color: #8A1F17; }
    QFrame#Toolbar { background: %(toolbar)s; border-bottom: 1px solid %(line)s; }
    QListWidget#Nav { background: white; border: none; border-right: 1px solid %(line)s; outline: 0;
                      padding-top: 10px; }
    QListWidget#Nav::item { padding: 11px 14px; margin: 3px 10px; border-radius: 8px; color: %(muted)s;
                            font-weight: 600; }
    QListWidget#Nav::item:selected { background: #E8EAF6; color: %(indigo_dark)s; }
    QListWidget#Nav::item:hover:!selected { background: #F3F4FB; }
    QFrame#Card { background: white; border: 1px solid %(line)s; border-radius: 12px; }
    QFrame#Warn { background: %(amber_soft)s; border: 1px solid %(orange)s; border-radius: 10px; }
    QFrame#Warn QLabel { color: %(amber_text)s; }
    QFrame#Badge { background: %(green_soft)s; border-radius: 12px; }
    QFrame#Badge QLabel { color: %(green_dark)s; }
    QLabel#H1 { font-size: 16pt; font-weight: 700; color: %(indigo_dark)s; }
    QLabel#H2 { font-size: 12pt; font-weight: 700; color: %(indigo_dark)s; }
    QLabel#Muted { color: %(muted)s; }
    QLabel#Info { color: %(blue_dark)s; }
    QLabel#Err { color: %(red_dark)s; }
    QLabel#Stat { font-size: 26pt; font-weight: 800; }
    QLabel#Empty { color: %(muted)s; font-size: 11pt; }
    QLineEdit, QComboBox, QPlainTextEdit { background: white; border: 1px solid %(line)s; border-radius: 6px;
                                           padding: 6px 8px; selection-background-color: %(indigo)s; }
    QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 2px solid %(indigo)s; padding: 5px 7px; }
    QPushButton { background: white; border: 1px solid %(line)s; border-radius: 6px; padding: 7px 16px;
                  color: %(indigo_dark)s; font-weight: 600; }
    QPushButton:hover { background: #EEF0FA; }
    QPushButton:pressed { background: #DDE2F5; }
    QPushButton:disabled { color: #A3A8BA; background: #F1F2F7; border-color: #E3E5EE; }
    QTableWidget { background: white; border: 1px solid %(line)s; border-radius: 8px; gridline-color: transparent;
                   alternate-background-color: #F5F6FD; selection-background-color: #C5CAE9;
                   selection-color: %(text)s; }
    QTableWidget::item { padding: 4px 8px; }
    QHeaderView::section { background: #EEF0FA; color: %(indigo_dark)s; font-weight: 700; padding: 8px;
                           border: none; border-bottom: 1px solid %(line)s; }
    QSplitter::handle { background: transparent; }
    QStatusBar { background: white; border-top: 1px solid %(line)s; color: %(muted)s; }
    QStatusBar QLabel { color: %(muted)s; padding: 0 8px; }
    QMenuBar { background: white; border-bottom: 1px solid %(line)s; }
    QMenuBar::item:selected { background: #E8EAF6; }
    QMenu { background: white; border: 1px solid %(line)s; }
    QMenu::item { padding: 6px 24px; }
    QMenu::item:selected { background: #E8EAF6; color: %(indigo_dark)s; }
    QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
    QScrollBar::handle:vertical { background: #C5CAE9; border-radius: 4px; min-height: 30px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
    QScrollBar::handle:horizontal { background: #C5CAE9; border-radius: 4px; min-width: 30px; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
    QToolTip { background: #263238; color: white; border: none; padding: 4px 8px; }
    """ % P
    for name, (c, dark) in VARIANTS.items():
        css += """
    QPushButton[variant="%(n)s"] { background: %(c)s; color: white; border: none; }
    QPushButton[variant="%(n)s"]:hover { background: %(d)s; }
    QPushButton[variant="%(n)s"]:pressed { background: %(d)s; }
    QPushButton[variant="%(n)s"]:disabled { background: #C9CDDA; color: #F5F6FA; }
    """ % {"n": name, "c": P[c], "d": P[dark]}
    for kind, (bg, fg) in H.COLORS.items():
        css += """
    QFrame#Banner[kind="%(k)s"] { background: %(bg)s; border: 1px solid %(fg)s; border-radius: 8px; }
    QFrame#Banner[kind="%(k)s"] QLabel { color: %(fg)s; }
    QFrame#Banner[kind="%(k)s"] QPushButton#Close { background: transparent; border: none; color: %(fg)s;
                                                    padding: 2px 6px; }
    """ % {"k": kind, "bg": bg, "fg": fg}
    return css


# --------------------------------------------------------------------------
# Small builders
# --------------------------------------------------------------------------

if QApplication is not None:

    def restyle(w):
        """Apply the style sheet again after a property it matches on has changed."""
        w.style().unpolish(w)
        w.style().polish(w)

    def label(text="", name=None, wrap=False):
        l = QLabel(text)
        if name:
            l.setObjectName(name)
        l.setWordWrap(wrap)
        return l

    def button(text, variant=None, slot=None, icon=None, tip=None):
        b = QPushButton(text)
        if variant:
            b.setProperty("variant", variant)
        if icon is not None:
            b.setIcon(b.style().standardIcon(icon))
        if slot:
            b.clicked.connect(slot)
        if tip:
            b.setToolTip(tip)
        b.setCursor(Qt.CursorShape.PointingHandCursor)
        return b

    def card(name="Card", margins=(18, 16, 18, 16), shadow=True):
        f = QFrame()
        f.setObjectName(name)
        lay = QVBoxLayout(f)
        lay.setContentsMargins(*margins)
        lay.setSpacing(10)
        if shadow:
            fx = QGraphicsDropShadowEffect(f)
            fx.setBlurRadius(18)
            fx.setOffset(0, 2)
            fx.setColor(QColor(40, 53, 147, 28))
            f.setGraphicsEffect(fx)
        return f, lay

    def hbox(*widgets, spacing=8, margins=(0, 0, 0, 0)):
        """A row; None in @widgets is a stretch."""
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(*margins)
        lay.setSpacing(spacing)
        for x in widgets:
            if x is None:
                lay.addStretch(1)
            else:
                lay.addWidget(x)
        return w

    def app_icon():
        """A drawn icon (no image files to ship): an indigo tile with a white check mark."""
        pm = QPixmap(64, 64)
        pm.fill(Qt.GlobalColor.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(QColor(P["indigo"]))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(2, 2, 60, 60, 14, 14)
        pen = p.pen()
        pen.setStyle(Qt.PenStyle.SolidLine)
        pen.setColor(QColor("white"))
        pen.setWidth(7)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        p.setPen(pen)
        p.drawPolyline([QPoint(17, 33), QPoint(28, 44), QPoint(47, 22)])
        p.end()
        return QIcon(pm)

    def nav_icon(kind):
        """The sidebar's icons, drawn (no image files to ship): a list for lectures, a person for students."""
        pm = QPixmap(48, 48)
        pm.fill(Qt.GlobalColor.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(P["indigo"]))
        if kind == "lectures":
            for i, y in enumerate((10, 21, 32)):
                p.drawEllipse(6, y, 7, 7)
                p.drawRoundedRect(18, y + 1, 24 - (6 if i == 2 else 0), 5, 2.5, 2.5)
        else:
            p.drawEllipse(16, 6, 16, 16)
            p.drawRoundedRect(8, 25, 32, 18, 9, 9)
        p.end()
        return QIcon(pm)

    class BatteryIcon(QWidget):
        """
        The device's battery drawn as a phone shows it: a rounded body with a nub on the right, filled from the
        left by the charge in its colour (green, orange, red: H.battery_level()), and the percentage beside it.
        Sized from the font; drawn in @fg, or the palette's text colour. Hidden while there is no reading.
        """

        def __init__(self, fg=None):
            QWidget.__init__(self)
            self.level = None
            self.fg = QColor(fg) if fg else None
            f = QFont(self.font())
            f.setBold(True)
            self.setFont(f)
            self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            self.hide()

        def set_level(self, level):
            """@level: H.battery_level()'s (percent, mv, colour), or None to hide it."""
            self.setToolTip(H.battery_tip(level) if level else "")
            if level != self.level:
                self.level = level
                self.updateGeometry()
                self.update()
            self.setVisible(bool(level))

        def _geometry(self):
            fm = self.fontMetrics()
            h = fm.height()
            bh = max(10, round(h * 0.8))                  # the body, about the height of a capital letter
            return h, bh, round(bh * 1.9), max(2, round(bh * 0.14)), round(h * 0.4), fm.horizontalAdvance("100 %")

        def sizeHint(self):
            h, bh, bw, nub, gap, tw = self._geometry()
            return QSize(bw + nub + gap + tw + 4, h + 6)

        def minimumSizeHint(self):
            return self.sizeHint()

        def paintEvent(self, e):
            if not self.level:
                return
            pct, _, colour = self.level
            h, bh, bw, nub, gap, tw = self._geometry()
            fg = self.fg or self.palette().color(QPalette.ColorRole.WindowText)
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            line = max(1.4, bh / 9.0)
            body = QRectF(1 + line / 2, (self.height() - bh) / 2.0, bw, bh)
            p.setPen(QPen(fg, line))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(body, bh * 0.25, bh * 0.25)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(fg)
            p.drawRoundedRect(QRectF(body.right() + line / 2, body.top() + bh * 0.3, nub, bh * 0.4), nub * 0.5, nub * 0.5)
            if pct > 0:
                inset = line + 1.2
                inner = body.adjusted(inset, inset, -inset, -inset)
                inner.setWidth(max(2.0, inner.width() * pct / 100.0))   # an almost empty cell still shows a sliver
                p.setBrush(QColor(P[colour]))
                p.drawRoundedRect(inner, bh * 0.1, bh * 0.1)
            p.setPen(fg)
            p.drawText(QRectF(body.right() + nub + gap, 0, tw + 4, self.height()),
                       int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter), "%d %%" % pct)
            p.end()

    # ----------------------------------------------------------------- table
    class DataTable(QTableWidget):
        """A read-only, striped, sortable table of rows that each carry a key.
        columns: [(heading, width)], width None stretches; numbers sort as numbers."""

        def __init__(self, columns, sortable=True, empty="", sort=None):
            QTableWidget.__init__(self, 0, len(columns))
            self.setHorizontalHeaderLabels([c[0] for c in columns])
            self.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
            self.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
            self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
            self.setAlternatingRowColors(True)
            self.setShowGrid(False)
            self.setWordWrap(False)
            self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
            self.verticalHeader().setVisible(False)
            self.verticalHeader().setDefaultSectionSize(34)
            h = self.horizontalHeader()
            h.setHighlightSections(False)
            h.setDefaultAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            for i, (_, width) in enumerate(columns):
                if width is None:
                    h.setSectionResizeMode(i, QHeaderView.ResizeMode.Stretch)
                else:
                    h.setSectionResizeMode(i, QHeaderView.ResizeMode.Interactive)
                    self.setColumnWidth(i, width)
            self.sortable = sortable
            if sortable:            # the order shown at first: (column, descending?); the user can click a heading
                col_, desc = sort or (0, False)
                h.setSortIndicator(col_, Qt.SortOrder.DescendingOrder if desc else Qt.SortOrder.AscendingOrder)
            self.setSortingEnabled(sortable)
            self.empty = QLabel(empty, self.viewport())
            self.empty.setObjectName("Empty")
            self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.empty.setWordWrap(True)
            self.empty.hide()
            self.on_open = None
            self.doubleClicked.connect(lambda idx: self._open())

        def fill(self, rows, empty=None):
            """rows: [(key, values, kind)]; kind 'running' shows the row in bold green. The selection is kept."""
            keep = self.selected_key()
            self.setSortingEnabled(False)
            self.setRowCount(len(rows))
            bold = QFont(self.font())
            bold.setBold(True)
            for r, (key, values, kind) in enumerate(rows):
                for c, v in enumerate(values):
                    it = QTableWidgetItem()
                    it.setData(Qt.ItemDataRole.DisplayRole, v if isinstance(v, int) else ("" if v is None else str(v)))
                    if c == 0:
                        it.setData(Qt.ItemDataRole.UserRole, key)
                    if kind == "running":
                        it.setForeground(QColor(P["green_dark"]))
                        it.setFont(bold)
                    if isinstance(v, int):
                        it.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                    self.setItem(r, c, it)
            self.setSortingEnabled(self.sortable)
            if empty is not None:
                self.empty.setText(empty)
            self.empty.setVisible(not rows and bool(self.empty.text()))
            self._place_empty()
            if keep is not None:
                self.select_key(keep)

        def _place_empty(self):
            vp = self.viewport()
            self.empty.setGeometry(20, 10, max(100, vp.width() - 40), max(60, vp.height() - 20))

        def resizeEvent(self, e):
            QTableWidget.resizeEvent(self, e)
            self._place_empty()

        def key_at(self, row):
            it = self.item(row, 0)
            return it.data(Qt.ItemDataRole.UserRole) if it is not None else None

        def selected_key(self):
            rows = self.selectionModel().selectedRows() if self.selectionModel() else []
            return self.key_at(rows[0].row()) if rows else None

        def select_key(self, key):
            for r in range(self.rowCount()):
                if self.key_at(r) == key:
                    self.selectRow(r)
                    self.scrollToItem(self.item(r, 0))
                    return True
            return False

        def keys(self):
            return [self.key_at(r) for r in range(self.rowCount())]

        def _open(self):
            k = self.selected_key()
            if k is not None and self.on_open:
                self.on_open(k)

        def keyPressEvent(self, e):
            if e.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                self._open()
                return
            QTableWidget.keyPressEvent(self, e)

    # ---------------------------------------------------------------- dialogs
    class Dialog(QDialog):
        def __init__(self, win, title, parent=None):
            QDialog.__init__(self, parent or win)
            self.win = win
            self.setWindowTitle(title)
            self.setModal(True)
            self.lay = QVBoxLayout(self)
            self.lay.setContentsMargins(22, 20, 22, 18)
            self.lay.setSpacing(8)
            self.lay.addWidget(label(title, "H2"))

        def buttons(self, primary, danger=None):
            row = []
            if danger:
                row.append(button(danger[0], "danger", danger[1]))
            row.append(None)
            row.append(button("Cancel", slot=self.reject))
            ok = button(primary, "primary", self.ok)
            ok.setDefault(True)
            row.append(ok)
            self.lay.addSpacing(8)
            self.lay.addWidget(hbox(*row))

        def ok(self):
            pass

        def fail(self, e, lbl):
            lbl.setText(H.err_text(e))
            QApplication.beep()

    class StudentDialog(Dialog):
        """Register a card, or edit a student: card number, index number and name."""

        def __init__(self, win, card=None, prefill=None):
            s = win.db.get_student(card) if card is not None else None
            Dialog.__init__(self, win, "Edit student" if s else "Register a student")
            self.student = s
            v = s or prefill or {}
            self.lay.addSpacing(6)
            self.lay.addWidget(label("Card number"))
            self.e_card = QLineEdit(H.card10(v["card_id"]) if v.get("card_id") else "")
            self.e_card.setPlaceholderText("0000123456")
            self.e_card.setMaximumWidth(180)
            row = [self.e_card, button("Tap card on device\u2026", slot=self.tap,
                                       tip="Tap the card on the device while it is plugged in to this computer")]
            self.pick = None
            if s:
                self.e_card.setReadOnly(True)          # a new card comes by tapping it; saving moves the student
            else:
                if win.cards:
                    self.pick = QComboBox()
                    self.pick.addItem("Pick a card the device saw\u2026")
                    for c in win.cards:
                        self.pick.addItem("%s  (last %s)" % (H.card10(c["card_id"]), c["last"][5:16]), c["card_id"])
                    self.pick.currentIndexChanged.connect(self._picked)
                    row.append(self.pick)
            row.append(None)
            self.lay.addWidget(hbox(*row))
            self.card_note = label("", "Info", wrap=True)
            self.card_note.hide()
            self.lay.addWidget(self.card_note)
            self.card_err = label("", "Err")
            self.lay.addWidget(self.card_err)
            self.e_card.textChanged.connect(self._check_card)

            grid = QGridLayout()
            grid.setHorizontalSpacing(12)
            grid.addWidget(label("Index number"), 0, 0)
            grid.addWidget(label("Name"), 0, 1)
            self.e_no = QLineEdit(v.get("student_no", ""))
            self.e_no.setMaxLength(40)
            self.e_no.setPlaceholderText("e.g. 220123A")
            self.e_name = QLineEdit(v.get("name", ""))
            self.e_name.setMaxLength(80)
            self.e_name.setPlaceholderText("Full name")
            self.e_name.setMinimumWidth(300)
            grid.addWidget(self.e_no, 1, 0)
            grid.addWidget(self.e_name, 1, 1)
            self.lay.addLayout(grid)
            self.err = label("", "Err", wrap=True)
            self.lay.addWidget(self.err)
            self.buttons("Save", ("Delete student", self.delete) if s else None)
            (self.e_no if (s or v.get("card_id")) else self.e_card).setFocus()

        def _picked(self, i):
            if i > 0:
                self.e_card.setText(H.card10(self.pick.itemData(i)))
                self.e_no.setFocus()

        def _check_card(self, v):
            v = v.strip()
            bad = v and not re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)", v)
            self.card_err.setText("A card number is digits only, like 0000123456." if bad else "")

        def tap(self):
            TapDialog(self.win, "Tap the card", self.tapped, parent=self).open()

        def tapped(self, card_id):
            """A card was tapped on the device: fill it in, unless it is someone else's (then keep waiting)."""
            owner = self.win.db.get_student(card_id)
            if owner and (not self.student or owner["card_id"] != self.student["card_id"]):
                return H.tap_owner_text(card_id, owner)
            self.e_card.setText(H.card10(card_id))
            if self.student and card_id != self.student["card_id"]:
                self.card_note.setText("A new card for this student: saving moves their taps and modules to it.")
                self.card_note.show()
            self.win.toast("Card %s read from the device" % H.card10(card_id))
            self.e_no.setFocus()
            return None

        def ok(self):
            self.err.setText("")
            dept = (self.student or {}).get("department", "")     # not shown here: kept as it was
            try:
                card = self.e_card.text()
                if self.student and D.parse_card(card) != self.student["card_id"]:
                    self.win.db.change_card(self.student["card_id"], card)
                s = self.win.db.upsert_student(card, self.e_name.text(), self.e_no.text(), dept, None)
            except D.DbError as e:
                return self.fail(e, self.err)
            self.accept()
            self.win.changed(s["name"] + " saved.", poll=True)

        def delete(self):
            if not self.win.ask("Delete this student? Their taps stay, and show as an unregistered card.", self):
                return
            try:
                self.win.db.delete_student(self.student["card_id"])
            except D.DbError as e:
                return self.fail(e, self.err)
            self.accept()
            self.win.changed("Student deleted")

    class TapDialog(Dialog):
        """
        Waits for a card tapped on the plugged-in device, reading LASTCARD.TXT every H.TAP_POLL_MS on the worker
        thread. Each new card goes to @on_card, which returns None when it took the card (this dialog then
        closes) or a message to show while waiting for another. Cancel stops the waiting.
        """

        def __init__(self, win, title, on_card, parent=None):
            Dialog.__init__(self, win, title, parent)
            self.on_card = on_card
            self.watch = H.TapWatch()
            self.closed = False
            intro = label(H.TAP_INTRO, wrap=True)
            intro.setMinimumWidth(420)
            self.lay.addWidget(intro)
            self.msg = label("Looking for the device\u2026", "Info", wrap=True)
            self.lay.addWidget(self.msg)
            self.lay.addSpacing(8)
            self.lay.addWidget(hbox(None, button("Cancel", slot=self.reject)))
            QTimer.singleShot(0, self._look)

        def _look(self):
            if not self.closed:
                self.win._job(self.win.app.last_card, self._got, self._failed, busy=False)

        def _again(self):
            if not self.closed:
                QTimer.singleShot(H.TAP_POLL_MS, self._look)

        def _say(self, text, kind="Info"):
            self.msg.setText(text)
            if self.msg.objectName() != kind:
                self.msg.setObjectName(kind)
                restyle(self.msg)

        def _got(self, lc):
            if self.closed:
                return
            card = self.watch.feed(lc)
            if card is None:
                self._say(H.tap_wait_text(lc, self.win.connected()))
            else:
                problem = self.on_card(card)
                if self.closed:
                    return
                if problem is None:
                    return self.accept()
                self._say(problem, "Err")
                QApplication.beep()
            self._again()

        def _failed(self, e):
            if not self.closed:
                self._say("Could not read the device: %s" % H.err_text(e), "Err")
                self._again()

        def done(self, r):
            self.closed = True
            QDialog.done(self, r)

    class EraseDialog(Dialog):
        """The strong confirmation for erasing the device without saving: the word ERASE must be typed."""

        def __init__(self, win):
            Dialog.__init__(self, win, "Erase the device without saving?")
            t = label(H.ERASE_TEXT, wrap=True)
            t.setMinimumWidth(460)
            self.lay.addWidget(t)
            self.lay.addSpacing(6)
            self.lay.addWidget(label("Type %s to confirm" % H.ERASE_WORD))
            self.e_word = QLineEdit()
            self.e_word.setMaximumWidth(180)
            self.lay.addWidget(self.e_word)
            self.b_erase = button("Erase without saving", "danger", self.ok)
            self.b_erase.setEnabled(False)
            self.e_word.textChanged.connect(lambda t: self.b_erase.setEnabled(t.strip() == H.ERASE_WORD))
            self.lay.addSpacing(8)
            self.lay.addWidget(hbox(None, button("Cancel", slot=self.reject), self.b_erase))
            self.e_word.setFocus()

        def ok(self):
            if self.e_word.text().strip() == H.ERASE_WORD:
                self.accept()

    class ClearDialog(Dialog):
        """The strong confirmation for clearing the records on this computer: the word CLEAR must be typed.
        also_device() says whether the device is to be cleared too."""

        def __init__(self, win):
            Dialog.__init__(self, win, "Clear all records on this computer?")
            t = label(H.CLEAR_TEXT, wrap=True)
            t.setMinimumWidth(460)
            self.lay.addWidget(t)
            self.lay.addSpacing(4)
            self.c_device = QCheckBox(H.CLEAR_DEVICE_TEXT)
            self.lay.addWidget(self.c_device)
            if not win.connected():
                self.c_device.setEnabled(False)
                self.lay.addWidget(label("The device is not connected.", "Muted"))
            self.lay.addSpacing(6)
            self.lay.addWidget(label("Type %s to confirm" % H.CLEAR_WORD))
            self.e_word = QLineEdit()
            self.e_word.setMaximumWidth(180)
            self.lay.addWidget(self.e_word)
            self.b_clear = button("Clear all records", "danger", self.ok)
            self.b_clear.setEnabled(False)
            self.e_word.textChanged.connect(lambda t: self.b_clear.setEnabled(t.strip() == H.CLEAR_WORD))
            self.lay.addSpacing(8)
            self.lay.addWidget(hbox(None, button("Cancel", slot=self.reject), self.b_clear))
            self.e_word.setFocus()

        def also_device(self):
            return self.c_device.isEnabled() and self.c_device.isChecked()

        def ok(self):
            if self.e_word.text().strip() == H.CLEAR_WORD:
                self.accept()

    class ConnectDialog(Dialog):
        """
        Connect with the device's drive gone: says how to bring it back, and looks for it every
        H.CONNECT_POLL_MS on the worker thread (H.ConnectWait) until it appears, which reads it and closes this,
        or the time is up. Cancel stops the looking.
        """

        def __init__(self, win):
            Dialog.__init__(self, win, "Connect the device")
            self.wait = H.ConnectWait()
            self.closed = False
            intro = label(H.CONNECT_TEXT, wrap=True)
            intro.setMinimumWidth(420)
            self.lay.addWidget(intro)
            self.msg = label(self.wait.text(), "Info", wrap=True)
            self.lay.addWidget(self.msg)
            self.lay.addSpacing(8)
            self.b_cancel = button("Cancel", slot=self.reject)
            self.lay.addWidget(hbox(None, self.b_cancel))
            QTimer.singleShot(0, self._look)

        def _look(self):
            if not self.closed:
                self.win._job(self.win.app.device_dir, self._got, self._failed, busy=False)

        def _say(self, text, kind="Info"):
            self.msg.setText(text)
            if self.msg.objectName() != kind:
                self.msg.setObjectName(kind)
                restyle(self.msg)

        def _got(self, root):
            if self.closed:
                return
            if root:
                self.accept()
                self.win.connect_found()
            elif self.wait.expired():
                self._say(H.CONNECT_TIMEOUT, "Err")
                self.b_cancel.setText("Close")
                QApplication.beep()
            else:
                self._say(self.wait.text())
                QTimer.singleShot(H.CONNECT_POLL_MS, self._look)

        def _failed(self, e):
            if not self.closed:
                self._say("Could not look for the device: %s" % H.err_text(e), "Err")
                QTimer.singleShot(H.CONNECT_POLL_MS, self._look)

        def done(self, r):
            self.closed = True
            self.win.connecting = None
            QDialog.done(self, r)
            self.win.render_chrome()

    class ImportDialog(Dialog):
        """A class list pasted from Excel or read from a CSV file."""

        def __init__(self, win):
            Dialog.__init__(self, win, "Import students")
            self.lay.addWidget(label("Paste rows from Excel, or choose a CSV file, with the columns: card number, "
                                     "name, index number. A first line naming the columns (card_id, name, index_no) "
                                     "is fine too. Students already in the list are updated.", "Muted", wrap=True))
            self.text = QPlainTextEdit()
            self.text.setFont(QFont("monospace"))
            self.text.setMinimumSize(560, 220)
            self.lay.addWidget(self.text)
            self.file_lbl = label("", "Muted")
            self.lay.addWidget(hbox(button("Choose a file\u2026", slot=self.pick), self.file_lbl, None))
            self.result = label("", wrap=True)
            self.lay.addWidget(self.result)
            self.buttons("Import")
            self.text.setFocus()

        def pick(self):
            path, _ = QFileDialog.getOpenFileName(self, "Choose a class list", "",
                                                  "CSV or text (*.csv *.CSV *.txt *.TXT);;All files (*)")
            if not path:
                return
            try:
                text = A.read_text(path)
            except OSError as e:
                return self.fail(e, self.result)
            self.text.setPlainText(text)
            self.file_lbl.setText(os.path.basename(path))

        def ok(self):
            text = self.text.toPlainText()
            if text.strip() and not re.match(r"^\s*[A-Za-z_]", text):
                # Columns by position: card, name, index number (the database's own order puts the number third).
                import csv
                import io
                rows = list(csv.reader(io.StringIO(text), delimiter="\t" if text.count("\t") > text.count(",") else ","))
                text = "card_id,name,index_no\n" + "\n".join(
                    ",".join(c.replace(",", " ") for c in (r + ["", "", ""])[:3]) for r in rows if any(x.strip() for x in r))
            try:
                r = self.win.db.import_students(text, replace_modules=False)
            except D.DbError as e:
                self.result.setObjectName("Err")
                restyle(self.result)
                return self.fail(e, self.result)
            msg = "%s added, %d updated." % (H.plural(r["added"], "student"), r["updated"])
            errs = ["Line %d: %s" % (x["line"], x["message"]) for x in r["errors"][:8]]
            if len(r["errors"]) > 8:
                errs.append("\u2026and %d more" % (len(r["errors"]) - 8))
            self.result.setObjectName("Err" if errs else "Info")
            restyle(self.result)
            self.result.setText("\n".join([msg] + errs))
            self.win.changed(None)

    # ----------------------------------------------------------------- window
    class MainWindow(QMainWindow):
        PAGES = ("lectures", "students")

        def __init__(self, app, data_dir=None, demo=False, poll=True):
            QMainWindow.__init__(self)
            self.app, self.db = app, app.db
            self.data_dir, self.demo = data_dir, demo
            self.closing = False
            self.results = queue.Queue()
            self.worker = H.Worker(self.results)      # device scans and writes, one at a time, in order
            self.busy_n = 0
            self.refreshing = 0
            self.state = None
            self.students, self.lectures, self.cards = [], [], []
            self.sent = None               # the lecture just sent, waiting for the device to confirm it
            self.dismissed = set()
            self.sending = False
            self.connecting = None         # the Connect dialog while it waits for the device
            self.title_touched = False
            self._quiet = False
            self.sel_lecture = None
            self.detail = None
            self.banner_sig = None
            self.prefs = H.load_prefs(data_dir)

            self.setWindowTitle(APP_TITLE + (" (demo)" if demo else ""))
            self.setWindowIcon(app_icon())
            self.resize(1180, 760)
            self.setMinimumSize(940, 620)
            self._build()
            self._menus()
            self.reload()
            self.render()

            self.drain_timer = QTimer(self)
            self.drain_timer.timeout.connect(self._drain)
            self.drain_timer.start(100)
            self.poll_timer = QTimer(self)
            self.poll_timer.timeout.connect(self.poll)
            if poll:
                self.poll()
                self.poll_timer.start(POLL_MS)

        # -------------------------------------------------------- plumbing
        def _drain(self):
            try:
                while True:
                    cb, value = self.results.get_nowait()
                    if cb and not self.closing:
                        cb(value)
            except queue.Empty:
                pass

        def set_busy(self, on):
            if on:
                self.busy_n += 1
                if self.busy_n == 1:
                    QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
            elif self.busy_n:
                self.busy_n -= 1
                if self.busy_n == 0:
                    QApplication.restoreOverrideCursor()

        def _job(self, fn, done=None, failed=None, busy=True):
            if busy:
                self.set_busy(True)

            def ok(v):
                if busy:
                    self.set_busy(False)
                if done:
                    done(v)

            def bad(e):
                if busy:
                    self.set_busy(False)
                (failed or self.show_error)(e)
            self.worker.submit(fn, ok, bad)

        def show_error(self, e, parent=None):
            QMessageBox.critical(parent or self, APP_TITLE, H.err_text(e))

        def ask(self, text, parent=None):
            return QMessageBox.question(parent or self, APP_TITLE, text) == QMessageBox.StandardButton.Yes

        def toast(self, msg):
            if msg:
                self.statusBar().showMessage(msg, 7000)
            else:
                self.statusBar().clearMessage()

        def connected(self):
            return bool(self.state and self.state.get("connected"))

        def current_page(self):
            return self.PAGES[self.stack.currentIndex()]

        # ------------------------------------------------------------ build
        def _build(self):
            root = QWidget()
            outer = QVBoxLayout(root)
            outer.setContentsMargins(0, 0, 0, 0)
            outer.setSpacing(0)
            self.setCentralWidget(root)

            # header: name on the left, the device's state on the right
            head = QFrame()
            head.setObjectName("Header")
            hl = QHBoxLayout(head)
            hl.setContentsMargins(22, 14, 22, 14)
            logo = QLabel()
            logo.setPixmap(app_icon().pixmap(40, 40))
            hl.addWidget(logo)
            tb = QVBoxLayout()
            tb.setSpacing(0)
            tb.addWidget(label(APP_TITLE, "AppTitle"))
            tb.addWidget(label("RFID attendance for your lectures", "AppSub"))
            hl.addLayout(tb)
            hl.addStretch(1)
            self.batt = BatteryIcon("white")          # shown while a device reports its battery
            hl.addWidget(self.batt)
            hl.addSpacing(12)
            self.chip = label("", "Chip")
            hl.addWidget(self.chip)
            outer.addWidget(head)

            # the device buttons
            bar = QFrame()
            bar.setObjectName("Toolbar")
            bl = QHBoxLayout(bar)
            bl.setContentsMargins(22, 10, 22, 10)
            bl.setSpacing(10)
            self.b_connect = button("\u21c4  Connect", "primary", self.connect,
                                    tip="Read the device now; if its drive is gone (after an eject), wait for it to "
                                        "come back")
            bl.addWidget(self.b_connect)
            bl.addWidget(button("\u21bb  Read device", "info", lambda: self.poll(force=True),
                                tip="Read the taps and lectures off the device now (F5)"))
            self.b_eject = button("\u23cf  Eject", "warning", self.eject,
                                  tip="Eject the drive safely: the device then takes attendance with the cable still in")
            bl.addWidget(self.b_eject)
            self.b_clock = button("\u25f7  Set device time", "teal", self.set_clock,
                                  tip="Set the device clock to this computer's time")
            bl.addWidget(self.b_clock)
            self.clock_lbl = label("", "Muted")
            bl.addWidget(self.clock_lbl)
            bl.addStretch(1)
            self.b_clear = button("\u2715  Clear device records", "danger", self.clear_device,
                                  tip="Copy every record to this computer, then erase them from the device")
            bl.addWidget(self.b_clear)
            outer.addWidget(bar)

            # body: navigation on the left, the pages on the right
            body = QWidget()
            bod = QHBoxLayout(body)
            bod.setContentsMargins(0, 0, 0, 0)
            bod.setSpacing(0)
            self.nav = QListWidget()
            self.nav.setObjectName("Nav")
            self.nav.setFixedWidth(200)
            self.nav.setIconSize(self.nav.iconSize() * 1.2)
            for text, kind in (("Lectures", "lectures"), ("Students", "students")):
                QListWidgetItem(nav_icon(kind), "  " + text, self.nav)
            bod.addWidget(self.nav)
            right = QWidget()
            right.setObjectName("Page")
            rl = QVBoxLayout(right)
            rl.setContentsMargins(0, 0, 0, 0)
            rl.setSpacing(0)
            self.banner_box = QWidget()
            self.banner_lay = QVBoxLayout(self.banner_box)
            self.banner_lay.setContentsMargins(22, 14, 22, 0)
            self.banner_lay.setSpacing(8)
            self.banner_box.hide()
            rl.addWidget(self.banner_box)
            self.stack = QStackedWidget()
            self.stack.addWidget(self._build_lectures())
            self.stack.addWidget(self._build_students())
            rl.addWidget(self.stack, 1)
            bod.addWidget(right, 1)
            outer.addWidget(body, 1)
            self.nav.currentRowChanged.connect(self._page_changed)
            self.nav.setCurrentRow(0)

            sb = self.statusBar()
            sb.setSizeGripEnabled(True)
            self.counts_lbl = QLabel()
            sb.addPermanentWidget(self.counts_lbl)

        def _menus(self):
            mb = self.menuBar()
            sp = QStyle.StandardPixmap

            def act(menu, text, slot, key=None, icon=None):
                a = QAction(self.style().standardIcon(icon) if icon is not None else QIcon(), text, self)
                if key:
                    a.setShortcut(QKeySequence(key))
                a.triggered.connect(slot)
                menu.addAction(a)
                return a
            f = mb.addMenu("&File")
            ex = f.addMenu("&Export")
            for what, kind, text in H.EXPORTS:
                act(ex, text, lambda checked=False, w=what, k=kind: self.export(w, k))
            act(f, "&Back up the database\u2026", self.backup, "Ctrl+B", sp.SP_DialogSaveButton)
            f.addSeparator()
            act(f, "&Clear all records on this computer\u2026", self.clear_records, icon=sp.SP_TrashIcon)
            f.addSeparator()
            act(f, "&Quit", self.close, "Ctrl+Q")
            d = mb.addMenu("&Device")
            self.a_connect = act(d, "&Connect", self.connect, "Ctrl+K")
            act(d, "&Read now", lambda: self.poll(force=True), "F5", sp.SP_BrowserReload)
            self.a_clock = act(d, "Set device &time", self.set_clock)
            self.a_eject = act(d, "&Eject", self.eject, "Ctrl+E")
            d.addSeparator()
            self.a_clear = act(d, "C&lear device records\u2026", self.clear_device, icon=sp.SP_TrashIcon)
            self.a_erase = act(d, "E&rase device without saving\u2026", self.erase_device,
                               icon=sp.SP_MessageBoxWarning)
            v = mb.addMenu("&View")
            act(v, "&Lectures", lambda: self.nav.setCurrentRow(0), "Ctrl+1")
            act(v, "&Students", lambda: self.nav.setCurrentRow(1), "Ctrl+2")
            act(v, "&Find", self.focus_search, "Ctrl+F")
            h = mb.addMenu("&Help")
            act(h, "&About", self.about)

        def about(self):
            QMessageBox.about(self, "About " + APP_TITLE, "<b>%s</b><p>Reads the attendance logger over USB, keeps your "
                              "students and lectures, and shows who came to each lecture.</p><p>Database: %s</p>"
                              % (APP_TITLE, getattr(self.db, "path", "")))

        def focus_search(self):
            e = self.e_st_q if self.current_page() == "students" else self.e_lec_q
            e.setFocus()
            e.selectAll()

        # ---- lectures page
        def _build_lectures(self):
            page = QWidget()
            page.setObjectName("Page")
            lay = QVBoxLayout(page)
            lay.setContentsMargins(22, 16, 22, 16)
            lay.setSpacing(14)

            start, sl = card()
            sl.addWidget(label("Start a lecture", "H2"))
            self.e_title = QLineEdit()
            self.e_title.setPlaceholderText("Lecture name, e.g. Circuits Lecture 5")
            self.e_title.setMaxLength(D.LECTURE_BYTES)
            self.e_title.setMinimumWidth(340)
            self.e_title.returnPressed.connect(self.start_lecture)
            self.e_title.textEdited.connect(self._title_edited)
            self.b_send = button("\u25b6  Start lecture", "success", self.start_lecture)
            self.send_why = label("", "Muted")
            sl.addWidget(hbox(self.e_title, self.b_send, self.send_why, None, spacing=10))
            sl.addWidget(label("Saves the device's taps here, sets its clock and starts the lecture. No computer at "
                               "hand? Hold the device's button until it buzzes (2 seconds) and let go: the next "
                               "lecture starts, and appears here the next time you plug the device in.", "Muted",
                               wrap=True))
            lay.addWidget(start)

            split = QSplitter(Qt.Orientation.Horizontal)
            split.setHandleWidth(14)
            split.setChildrenCollapsible(False)

            left, ll = card()
            self.lec_count = label("", "Muted")
            ll.addWidget(hbox(label("Lectures", "H2"), self.lec_count, None,
                              button("Clear all records\u2026", slot=self.clear_records,
                                     tip="Delete every lecture and tap from this computer (the students stay), after "
                                         "saving a copy of the database")))
            self.lec_table = DataTable([("Lecture", None), ("Date", 96), ("Start", 58), ("Present", 72)],
                                       sortable=False,
                                       empty="No lectures yet. Start one above, or hold the device's button for 2 "
                                             "seconds.")
            self.lec_table.itemSelectionChanged.connect(self._lecture_picked)
            ll.addWidget(self.lec_table)
            split.addWidget(left)

            right, rl = card()
            self.d_stack = QStackedWidget()
            self.d_stack.addWidget(label("Pick a lecture to see who came.", "Empty"))
            detail = QWidget()
            dl = QVBoxLayout(detail)
            dl.setContentsMargins(0, 0, 0, 0)
            dl.setSpacing(10)
            self.l_title = label("", "H1")
            self.l_sub = label("", "Muted")
            tv = QWidget()
            tvl = QVBoxLayout(tv)
            tvl.setContentsMargins(0, 0, 0, 0)
            tvl.setSpacing(2)
            tvl.addWidget(self.l_title)
            tvl.addWidget(self.l_sub)
            badge = QFrame()
            badge.setObjectName("Badge")
            bgl = QVBoxLayout(badge)
            bgl.setContentsMargins(18, 6, 18, 8)
            bgl.setSpacing(0)
            self.l_num = label("0", "Stat")
            self.l_num.setAlignment(Qt.AlignmentFlag.AlignCenter)
            pl = label("present")
            pl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            bgl.addWidget(self.l_num)
            bgl.addWidget(pl)
            dl.addWidget(hbox(tv, None, badge))
            self.e_lec_q = QLineEdit()
            self.e_lec_q.setPlaceholderText("Search by name or index number")
            self.e_lec_q.setClearButtonEnabled(True)
            self.e_lec_q.textChanged.connect(lambda t: self.draw_detail())
            dl.addWidget(self.e_lec_q)
            self.l_table = DataTable([("Index No", 140), ("Name", None), ("Time", 90)], sort=(2, False))
            self.l_table.on_open = lambda k: StudentDialog(self, k).open()
            dl.addWidget(self.l_table, 1)
            self.unreg, ul = card("Warn", (14, 8, 10, 8), shadow=False)
            self.unreg_lbl = label("")
            self.unreg_lbl.setStyleSheet("font-weight: 600;")
            ul.addWidget(hbox(self.unreg_lbl, None, button("Register\u2026", "warning", self.register_from_lecture)))
            dl.addWidget(self.unreg)
            self.b_end = button("End lecture", slot=self.end_lecture)
            dl.addWidget(hbox(button("Rename\u2026", slot=self.rename_lecture), self.b_end,
                              button("Delete", "danger", self.delete_lecture), None,
                              button("Save list (CSV)\u2026", "primary", lambda: self.save_lecture("csv")),
                              button("Save as PDF\u2026", "primary", lambda: self.save_lecture("pdf"))))
            self.d_stack.addWidget(detail)
            rl.addWidget(self.d_stack)
            split.addWidget(right)
            split.setStretchFactor(0, 2)
            split.setStretchFactor(1, 3)
            split.setSizes([520, 560])
            lay.addWidget(split, 1)
            return page

        # ---- students page
        def _build_students(self):
            page = QWidget()
            page.setObjectName("Page")
            lay = QVBoxLayout(page)
            lay.setContentsMargins(22, 16, 22, 16)
            lay.setSpacing(14)

            self.newcards, nl = card("Warn", (16, 12, 16, 12), shadow=False)
            self.nc_title = label("")
            self.nc_title.setStyleSheet("font-size: 12pt; font-weight: 700;")
            nl.addWidget(self.nc_title)
            nl.addWidget(label("These cards were tapped but belong to nobody yet. Register each one with an index "
                               "number and a name.", wrap=True))
            self.nc_table = DataTable([("Card", 150), ("Tapped", 90), ("Last tap", None)], sort=(2, True))
            self.nc_table.setMaximumHeight(150)
            self.nc_table.on_open = self.register
            reg = button("Register\u2026", "warning",
                         lambda: self.nc_table.selected_key() is not None and self.register(self.nc_table.selected_key()))
            row = QWidget()
            rwl = QHBoxLayout(row)
            rwl.setContentsMargins(0, 0, 0, 0)
            rwl.addWidget(self.nc_table, 1)
            rwl.addWidget(reg, 0, Qt.AlignmentFlag.AlignTop)
            nl.addWidget(row)
            lay.addWidget(self.newcards)

            main, ml = card()
            self.st_count = label("", "Muted")
            self.b_tap = button("Tap a card\u2026", "purple", self.tap_card,
                                tip="Tap a card on the plugged-in device: with a student picked in the list, it "
                                    "becomes their card; with none, a new student is registered with it (or you "
                                    "see whose it is)")
            ml.addWidget(hbox(label("Students", "H1"), self.st_count, None,
                              button("+  Add student", "success", lambda: StudentDialog(self, None, {}).open()),
                              button("Import list\u2026", slot=lambda: ImportDialog(self).open()), self.b_tap))
            self.e_st_q = QLineEdit()
            self.e_st_q.setPlaceholderText("Search by name, index number or card")
            self.e_st_q.setClearButtonEnabled(True)
            self.e_st_q.textChanged.connect(lambda t: self.draw_students())
            ml.addWidget(self.e_st_q)
            self.st_table = DataTable([("Index No", 150), ("Name", None), ("Card", 140), ("Last tap", 170)],
                                      sort=(0, False))
            self.st_table.on_open = lambda k: StudentDialog(self, k).open()
            ml.addWidget(self.st_table, 1)
            help_lbl = label("New student: with the device plugged in, press Tap a card\u2026 and tap their card on it. "
                             "Cards tapped while it was unplugged appear above, to Register.", "Muted", wrap=True)
            help_lbl.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
            ml.addWidget(hbox(button("Edit\u2026", slot=self.edit_student), button("Delete", "danger",
                                                                                  self.delete_student), help_lbl,
                              spacing=10))
            lay.addWidget(main, 1)
            QShortcut(QKeySequence(Qt.Key.Key_Delete), self.st_table, self.delete_student)
            return page

        # ------------------------------------------------------------ device
        def poll(self, force=False):
            """Look at the device on the worker thread (the timer calls this every POLL_MS)."""
            if self.closing:
                return
            if force or not self.refreshing:
                self.refreshing += 1
                if force:
                    self.toast("Reading the device\u2026")
                self._job(lambda: self.app.refresh(force), self._got_state, self._poll_failed, busy=force)

        def _poll_failed(self, e):
            self.refreshing = max(0, self.refreshing - 1)
            self.toast("Could not read the device: %s" % H.err_text(e))

        def _got_state(self, st):
            self.refreshing = max(0, self.refreshing - 1)
            prev, self.state = self.state, st
            changed = prev is None or prev["connected"] != st["connected"] or prev["sync"]["seq"] != st["sync"]["seq"]
            if prev and st["sync"]["seq"] != prev["sync"]["seq"] and (st["sync"]["new"] or st["sync"].get("lectures")):
                bits = [H.plural(st["sync"]["new"], "new tap")]
                if st["sync"].get("lectures"):
                    bits.append(H.plural(st["sync"]["lectures"], "new lecture"))
                self.toast(" and ".join(bits) + " read from the device")
            elif self.statusBar().currentMessage() == "Reading the device\u2026":
                self.toast("The device was read at %s" % st["sync"]["at"] if st["connected"]
                           else "The device is not connected.")
            if st["connected"]:
                d = st.get("device")
                s = self.sent
                if s and d and d["has_lecture"] and d["module"] == s["module"] and d["lecture"] == s["title"] \
                        and not d["pending"]:
                    s["done"] = True
            if changed:
                self.reload()
                self.render()
            else:
                self.render_chrome()

        def set_clock(self):
            if not self.connected():
                return

            def done(r):
                self.sent = None
                self.toast("Device time set to this computer's. The device has it now." if r["ejected"] else
                           "Time sent. Eject the drive or press the device's button to apply it." + H.not_ejected_note(r))
                self.poll()
            self._job(self.app.set_clock_and_eject, done)

        def clear_device(self):
            if not self.connected():
                return
            box = QMessageBox(QMessageBox.Icon.Warning, "Clear device records",
                              "Clear the records in the device's memory?", parent=self)
            box.setInformativeText("Every tap and lecture is first copied to this computer; only then does the device "
                                   "erase them. The students and everything already on this computer are kept.")
            yes = box.addButton("Copy, then clear", QMessageBox.ButtonRole.AcceptRole)
            yes.setProperty("variant", "danger")
            box.addButton(QMessageBox.StandardButton.Cancel)
            box.exec()
            if box.clickedButton() is not yes:
                return

            def done(r):
                self.sent = None
                self.changed("All records copied here and erased from the device." if r.get("ejected") else
                             "Records copied here. Eject the drive or press the device's button: it then erases them."
                             + H.not_ejected_note(r), poll=True)
            self._job(self.app.clear_device, done)

        def erase_device(self):
            """Erase the device's records without copying them here: behind a typed confirmation."""
            if not self.connected():
                return
            dlg = EraseDialog(self)
            if dlg.exec() != QDialog.DialogCode.Accepted:
                return

            def done(r):
                self.sent = None
                self.changed("The device's records were erased without saving." if r.get("ejected") else
                             "Erase sent. Eject the drive or press the device's button: it then erases its records."
                             + H.not_ejected_note(r), poll=True)
            self._job(self.app.erase_device, done)

        def eject(self):
            if not self.connected():
                return

            def done(r):
                if r["ejected"]:
                    self.toast(H.EJECTED_TEXT)
                else:
                    QMessageBox.warning(self, APP_TITLE, H.eject_failed_text(r["eject_error"]))
                self.poll()
            self._job(lambda: {"ejected": self.app.eject(), "eject_error": self.app.last_eject_error}, done)

        def connect(self):
            """Read the device now when its drive is here; otherwise say how to bring it back, and wait for it."""
            if self.connecting is not None:
                return
            if self.connected():
                return self.poll(force=True)
            self.connecting = ConnectDialog(self)
            self.render_chrome()
            self.connecting.open()

        def connect_found(self):
            """The Connect dialog saw the drive come back: read it."""
            self.toast("Device connected.")
            self.poll(force=True)

        def clear_records(self):
            """Delete every lecture and tap on this computer (the students stay): behind a typed confirmation."""
            dlg = ClearDialog(self)
            if dlg.exec() != QDialog.DialogCode.Accepted:
                return
            also = dlg.also_device() and self.connected()

            def done(r):
                self.sent, self.sel_lecture, self.detail = None, None, None
                self.changed("Records on this computer cleared. Backup: %s" % r["backup"], poll=True)
                QMessageBox.information(self, APP_TITLE, H.clear_done_text(r))
            self._job(lambda: self.app.clear_records(also), done)

        # -------------------------------------------------------------- data
        def reload(self):
            db = self.db
            try:
                self.students = db.list_students()
                self.lectures = db.list_lectures()
                self.cards = db.unregistered_cards()
            except Exception as e:
                self.toast("Could not read the database: %s" % e)
            self.refresh_selected()

        def refresh_selected(self):
            if self.sel_lecture is None and self.lectures:
                cur = self.db.current_lecture()
                self.sel_lecture = cur["id"] if cur else self.lectures[0]["id"]
            if self.sel_lecture is not None:
                try:
                    self.detail = self.db.lecture_attendance(self.sel_lecture)
                except D.DbError:
                    self.sel_lecture, self.detail = None, None
            else:
                self.detail = None

        def changed(self, msg, poll=False):
            if msg:
                self.toast(msg)
            self.reload()
            self.render()
            if poll:
                self.poll()

        def _page_changed(self, i):
            if i < 0:
                return
            self.stack.setCurrentIndex(i)
            if not self.closing and hasattr(self, "a_clear"):          # not while the window is still being built
                self.reload()
                self.render()

        # ------------------------------------------------------------ render
        def render(self):
            self.render_chrome()
            getattr(self, "render_" + self.current_page())()

        def render_chrome(self):
            st = self.state
            kind, title, detail = H.device_summary(st, self.demo)
            self.chip.setText("\u25cf  " + title + ("  \u00b7  " + detail if detail else ""))
            self.chip.setProperty("kind", kind)
            restyle(self.chip)
            self.batt.set_level(H.battery_level(st))
            on = self.connected()
            for w in (self.b_clock, self.b_clear, self.b_eject, self.a_clock, self.a_eject, self.a_clear, self.a_erase):
                w.setEnabled(on)
            for w in (self.b_connect, self.a_connect):
                w.setEnabled(self.connecting is None)
            self.clock_lbl.setText(H.lecture_clock_text(st))
            c = st["counts"] if st else self.db.counts()
            self.counts_lbl.setText(("Demo data: nothing here is real  \u00b7  " if self.demo else "") +
                                    "%s  \u00b7  %s  \u00b7  %s" % (H.plural(c["students"], "student"),
                                                                  H.plural(c["lectures"], "lecture"),
                                                                  H.plural(c["taps"], "tap")))
            self.render_banners()
            self._lecture_controls()

        def render_banners(self):
            items = H.banner_list(self.state, self.sent, self.current_page(), self.dismissed)
            sig = tuple((b["key"], b["title"], b["text"]) for b in items)
            if sig == self.banner_sig:
                return
            self.banner_sig = sig
            while self.banner_lay.count():
                w = self.banner_lay.takeAt(0).widget()
                if w is not None:
                    w.deleteLater()
            for b in items:
                f = QFrame()
                f.setObjectName("Banner")
                f.setProperty("kind", b["kind"])
                fl = QHBoxLayout(f)
                fl.setContentsMargins(14, 8, 8, 8)
                t = label(b["title"])
                t.setStyleSheet("font-weight: 700;")
                fl.addWidget(t, 0, Qt.AlignmentFlag.AlignTop)
                fl.addWidget(label(b["text"], wrap=True), 1)
                if b["action"] == "eject":
                    fl.addWidget(button("Eject now", "info", self.eject))
                else:
                    x = button("\u2715", slot=lambda checked=False, k=b["key"]: self.dismiss(k), tip="Dismiss")
                    x.setObjectName("Close")
                    fl.addWidget(x, 0, Qt.AlignmentFlag.AlignTop)
                self.banner_lay.addWidget(f)
                restyle(f)
            self.banner_box.setVisible(bool(items))

        def dismiss(self, key):
            self.dismissed.add(key)
            self.render_banners()

        # ---- lectures
        def render_lectures(self):
            if not self.title_touched:
                self.e_title.setText(H.suggest_title(self.lectures, None, self.sent))
            rows = []
            for l in self.lectures:                 # newest first
                c = l["counts"]
                rows.append((l["id"], (("\u25cf  " if l["running"] else "") + l["title"], l["date"],
                                       l["start_text"][11:16], c["present_enrolled"] + c["present_other"]),
                             "running" if l["running"] else ""))
            self._quiet = True
            self.lec_table.fill(rows)
            if self.sel_lecture is not None:
                self.lec_table.select_key(self.sel_lecture)
            self._quiet = False
            self.lec_count.setText(H.plural(len(self.lectures), "lecture") if self.lectures else "")
            self.render_detail()
            self._lecture_controls()

        def _lecture_picked(self):
            if self._quiet:
                return
            k = self.lec_table.selected_key()
            if k is not None and k != self.sel_lecture:
                self.sel_lecture = k
                self.e_lec_q.clear()
                self.refresh_selected()
                self.render_detail()

        def render_detail(self):
            d = self.detail
            if not d:
                self.d_stack.setCurrentIndex(0)
                return
            self.d_stack.setCurrentIndex(1)
            l = d["lecture"]
            self.l_title.setText(l["title"])
            when = "still running" if l["running"] else "until " + l["end_text"][11:16]
            self.l_sub.setText("%s  \u00b7  started %s  \u00b7  %s" % (H.nice_date(l["start_ts"]),
                                                                   l["start_text"][11:16], when))
            self.l_num.setText(str(len(H.present_students(d))))
            un = d["unregistered"]
            self.unreg_lbl.setText("%s tapped in this lecture but %s not registered yet." % (
                H.plural(len(un), "card"), "is" if len(un) == 1 else "are"))
            self.unreg.setVisible(bool(un))
            self.b_end.setVisible(bool(l["running"]))
            self.draw_detail()

        def draw_detail(self):
            qs = self.e_lec_q.text()
            everyone = H.present_students(self.detail)
            rows = [(s["card_id"], (s["student_no"] or "\u2014", s["name"], s["time"][:5]), "")
                    for s in everyone if H.matches(qs, [s["name"], s["student_no"], s["card_id"], H.card10(s["card_id"])])]
            self.l_table.fill(rows, "Nobody has tapped in this lecture yet." if not everyone
                              else "Nobody matches that search.")

        def _title_edited(self, text):
            self.title_touched = True
            self._lecture_controls()

        def _lecture_controls(self):
            t = self.e_title.text().strip()
            if self.sending:
                why = "Sending\u2026"
            elif not self.connected():
                why = "Plug in the device to start a lecture from here."
            elif not t:
                why = "Give the lecture a name."
            else:
                why = ""
            self.b_send.setEnabled(not why)
            self.send_why.setText(why)

        def start_lecture(self):
            if not self.b_send.isEnabled():
                return
            module, title = H.lecture_module(self.lectures), self.e_title.text()
            self.sending = True
            self._lecture_controls()

            def done(r):
                self.sending = False
                self.sent = {"module": r["lecture"]["module_code"], "title": r["lecture"]["title"], "done": False,
                             "ejected": r.get("ejected", False), "cleared": r.get("cleared", False),
                             "eject_error": r.get("eject_error", "")}
                self.dismissed = set()
                self.sel_lecture = r["lecture"]["id"]
                self.title_touched = False
                self.changed("Sent and ejected. The lecture starts on the device now." if r.get("ejected") else
                             "Sent. Now eject the drive or press the button on the device." + H.not_ejected_note(r),
                             poll=True)

            def failed(e):
                self.sending = False
                self.show_error(e)
                self.render()
            self._job(lambda: self.app.start_lecture(module, title, True), done, failed)

        def _lecture(self):
            return self.detail["lecture"] if self.detail else None

        def rename_lecture(self):
            l = self._lecture()
            if not l:
                return
            text, ok = QInputDialog.getText(self, "Rename lecture", "Lecture name", QLineEdit.EchoMode.Normal, l["title"])
            if not ok:
                return
            try:
                self.db.update_lecture(l["id"], title=text)
            except D.DbError as e:
                return self.show_error(e)
            self.changed("Lecture renamed")

        def end_lecture(self):
            l = self._lecture()
            if not l:
                return
            try:
                self.db.end_lecture(l["id"])
            except D.DbError as e:
                return self.show_error(e)
            self.changed("Lecture ended")

        def delete_lecture(self):
            l = self._lecture()
            if not l or not self.ask("Delete the lecture \u201c%s\u201d? The taps stay; they just stop counting as "
                                     "this lecture." % l["title"]):
                return
            try:
                self.db.delete_lecture(l["id"])
            except D.DbError as e:
                return self.show_error(e)
            self.sel_lecture, self.detail = None, None
            self.changed("Lecture deleted")

        def register_from_lecture(self):
            if self.detail and self.detail["unregistered"]:
                self.register(self.detail["unregistered"][0]["card_id"])

        def save_lecture(self, kind):
            """Save the lecture shown as a CSV or a PDF (H.lecture_file())."""
            if not self._lecture():
                return
            try:
                data, name = H.lecture_file(self.detail, kind)
            except Exception as e:
                return self.show_error(e)
            self.save_file(data, name)

        def export(self, what, kind):
            """Save a whole-database list (H.EXPORTS): every tap, or the students, as CSV or PDF."""
            try:
                data, name = H.export_file(self.db, what, kind)
            except Exception as e:
                return self.show_error(e)
            self.save_file(data, name)

        # ---- students
        def render_students(self):
            self.nc_title.setText("New cards to register (%d)" % len(self.cards))
            self.nc_table.fill([(c["card_id"], (H.card10(c["card_id"]), H.plural(c["taps"], "time"), c["last"]), "")
                                for c in self.cards])
            self.newcards.setVisible(bool(self.cards))
            self.draw_students()

        def draw_students(self):
            qs = self.e_st_q.text()
            rows = [s for s in self.students
                    if H.matches(qs, [s["name"], s["student_no"], s["card_id"], H.card10(s["card_id"])])]
            n = len(self.students)
            self.st_count.setText(H.plural(n, "student") if len(rows) == n else "%d of %s" % (len(rows),
                                                                                           H.plural(n, "student")))
            self.st_table.fill([(s["card_id"], (s["student_no"] or "\u2014", s["name"], H.card10(s["card_id"]),
                                                s["last_tap_text"] or "\u2014"), "") for s in rows],
                               "No students yet. Add one, import a class list, or tap a card on the device and "
                               "register it when it appears." if not n else "Nobody matches that search.")

        def register(self, card_id):
            StudentDialog(self, None, {"card_id": card_id}).open()

        def tap_card(self):
            """Tap a card on the plugged-in device: it becomes the picked student's card, or registers a new student."""
            k = self.st_table.selected_key()
            student = self.db.get_student(k) if k is not None else None

            def for_student(card_id):
                if card_id == student["card_id"]:
                    self.toast("That is already %s's card." % student["name"])
                    return None
                owner = self.db.get_student(card_id)
                if owner:
                    return H.tap_owner_text(card_id, owner)
                if not self.ask("Give %s the card %s instead of %s?\n\nTheir taps and modules move to the new card."
                                % (student["name"], H.card10(card_id), H.card10(student["card_id"])), dlg):
                    return None
                try:
                    self.db.change_card(student["card_id"], card_id)
                except D.DbError as e:
                    return H.err_text(e)
                self.changed("%s now has the card %s." % (student["name"], H.card10(card_id)))
                self.st_table.select_key(card_id)
                return None

            def new_card(card_id):
                owner = self.db.get_student(card_id)
                if owner:                          # a card someone has: show whose it is
                    self.e_st_q.clear()
                    self.st_table.select_key(card_id)
                    self.toast("Card %s belongs to %s." % (H.card10(card_id), owner["name"]))
                else:
                    QTimer.singleShot(0, lambda: StudentDialog(self, None, {"card_id": card_id}).open())
                return None

            title = ("New card for %s" % student["name"]) if student else "Tap a card"
            dlg = TapDialog(self, title, for_student if student else new_card)
            dlg.open()

        def edit_student(self):
            k = self.st_table.selected_key()
            if k is not None:
                StudentDialog(self, k).open()

        def delete_student(self):
            k = self.st_table.selected_key()
            if k is None or not self.ask("Delete this student? Their taps stay, and show as an unregistered card."):
                return
            try:
                self.db.delete_student(k)
            except D.DbError as e:
                return self.show_error(e)
            self.changed("Student deleted")

        # ------------------------------------------------------------- files
        def save_file(self, data, name, filt=None):
            filt = filt or {".csv": "CSV files (*.csv)", ".pdf": "PDF files (*.pdf)",
                            ".db": "Database files (*.db)"}.get(os.path.splitext(name)[1].lower(), "All files (*)")
            folder = self.prefs.get("save_dir")
            start = os.path.join(folder if folder and os.path.isdir(folder) else os.path.expanduser("~"), name)
            path, _ = QFileDialog.getSaveFileName(self, "Save " + name, start, filt + ";;All files (*)")
            if not path:
                return
            try:
                with open(path, "wb") as f:
                    f.write(data)
            except OSError as e:
                return self.show_error(e)
            self.prefs["save_dir"] = os.path.dirname(path)
            H.save_prefs(self.data_dir, self.prefs)
            self.toast("Saved %s" % path)

        def backup(self):
            try:
                data, ctype, name = A.backup_export(self.db)
            except (A.ApiError, D.DbError, OSError) as e:
                return self.show_error(e)
            self.save_file(data, name, "Database files (*.db)")

        # ------------------------------------------------------------- close
        def closeEvent(self, e):
            self.shutdown()
            QMainWindow.closeEvent(self, e)

        def shutdown(self):
            if self.closing:
                return
            self.closing = True
            self.poll_timer.stop()
            if self.connecting is not None:
                self.connecting.closed = True
            self.drain_timer.stop()
            self.worker.stop(5.0)
            while self.busy_n:
                self.set_busy(False)
            H.save_prefs(self.data_dir, self.prefs)


# --------------------------------------------------------------------------
# Starting up
# --------------------------------------------------------------------------

def venv_python():
    """The Python in the .venv next to this file, if there is one and it is not the one running."""
    venv = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".venv")
    # A venv's python is a link to the system one, so compare environments, not executables.
    if os.path.realpath(sys.prefix) == os.path.realpath(venv):
        return None
    for rel in (("bin", "python"), ("Scripts", "pythonw.exe"), ("Scripts", "python.exe")):
        exe = os.path.join(venv, *rel)
        if os.path.isfile(exe):
            return exe
    return None


def main(argv=None):
    if QApplication is None:
        # Started with a Python that lacks PySide6 (an editor's Run button, or python3 by hand): use the
        # .venv that start.sh / start.bat use, when it is there. The variable stops a loop if that one
        # lacks PySide6 too.
        exe = venv_python()
        if exe and not os.environ.get("ATTENDANCE_VENV_TRIED"):
            os.environ["ATTENDANCE_VENV_TRIED"] = "1"
            args = [exe, os.path.abspath(__file__)] + list(sys.argv[1:] if argv is None else argv)
            if os.name == "nt":
                import subprocess
                return subprocess.call(args)
            os.execv(exe, args)
        print("The Attendance Logger needs PySide6 (Qt), which is not installed for this Python (%s).\n"
              "Install it once, from the Companion folder:\n"
              "    python -m venv .venv\n"
              "    .venv/bin/pip install -r requirements.txt        (Windows: .venv\\Scripts\\pip install -r "
              "requirements.txt)\n"
              "then start the app with start.sh or start.bat. Or, into this Python: "
              "python -m pip install -r requirements.txt" % sys.executable, file=sys.stderr)
        return 1

    ap = argparse.ArgumentParser(description="Attendance Logger companion app (desktop window, Qt)")
    ap.add_argument("--device-dir", help="use this folder as the device instead of searching drives")
    ap.add_argument("--data-dir", help="where the database lives (default: %s)" % A.default_data_dir())
    ap.add_argument("--demo", action="store_true", help="use made-up data in temporary folders; touches nothing real")
    args = ap.parse_args(argv)

    forced, data_dir = args.device_dir, args.data_dir or A.default_data_dir()
    scratch = []
    if args.demo:
        data_dir = tempfile.mkdtemp(prefix="attendance-demo-data-")
        forced = tempfile.mkdtemp(prefix="attendance-demo-device-")
        scratch = [data_dir, forced]
        A.make_demo(data_dir, forced)
        print("Demo data in %s and %s (nothing real is touched)" % (data_dir, forced), flush=True)

    qapp = QApplication.instance() or QApplication(sys.argv[:1])
    qapp.setApplicationName(APP_TITLE)
    qapp.setStyle("Fusion")                  # the same look on Windows, macOS and Linux, under the style sheet
    qapp.setStyleSheet(style_sheet())
    qapp.setWindowIcon(app_icon())

    lock = db = None
    try:
        try:
            os.makedirs(data_dir, exist_ok=True)
        except OSError as e:
            QMessageBox.critical(None, APP_TITLE, "Could not use the folder %s: %s" % (data_dir, e))
            return 1
        lock = H.InstanceLock(data_dir)
        if not lock.acquire():
            lock = None
            QMessageBox.information(None, APP_TITLE, "The Attendance Logger is already open for %s. Use that window "
                                    "(look for it on the taskbar)." % data_dir)
            return 0
        try:
            db = D.Database(os.path.join(data_dir, "attendance.db"))
        except Exception as e:
            QMessageBox.critical(None, APP_TITLE, "Could not open the database: %s" % e)
            return 1
        if not args.demo:
            try:
                A.rotate_backup(db, data_dir)
            except OSError:
                pass
        print("Database: %s" % db.path, flush=True)
        win = MainWindow(A.App(db, forced, data_dir), data_dir, demo=args.demo)
        # Ctrl+C in the terminal, or being told to stop: close tidily. The timer lets Python see the signal.
        for name in ("SIGINT", "SIGTERM"):
            try:
                signal.signal(getattr(signal, name), lambda *a: QTimer.singleShot(0, win.close))
            except (ValueError, OSError, AttributeError):
                pass
        tick = QTimer()
        tick.timeout.connect(lambda: None)
        tick.start(300)
        win.show()
        rc = qapp.exec()
        win.shutdown()
        return rc
    finally:
        if db is not None:
            db.close()
        if lock is not None:
            lock.release()
        for d in scratch:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
