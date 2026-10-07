#!/usr/bin/env python3
"""
Attendance Logger companion app: finding the device, reading and writing its
files, and the actions and exports the window (attendance_qt.py) calls. Running
this file opens the window.

The device is a small USB drive called ATTENDANCE holding ATTEND.CSV (one row
per tap: date, time, card number), LECTURES.CSV (one row per lecture start; not
on older firmware), SETTINGS.CSV, STATUS.TXT and LASTCARD.TXT (the card last
tapped while it is plugged in; newer firmware only), plus a LECTURES folder with
one CSV per lecture, which this app does not need. It knows nothing about
people, and keeps no list of registered cards: it records every card. This
program keeps who each card belongs to, their department and modules, and the
lectures, in its own database, and matches the two.
"""
import datetime
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time

import attendance_db as D
import report_pages as R

STATUS_NAME = "STATUS.TXT"
ATTEND_NAME = "ATTEND.CSV"
LECTURES_NAME = "LECTURES.CSV"         # every lecture start in the log; older firmware has none
SETTINGS_NAME = "SETTINGS.CSV"
LASTCARD_NAME = "LASTCARD.TXT"         # the card last tapped while plugged in; older firmware has none
STATUS_MAGIC = "ATTENDANCE LOGGER"
MAX_SETTINGS_BYTES = 28 * 512         # the window on the device (SETF_MAX_BYTES)
BACKUPS_KEPT = 14

parse_last_card = D.parse_last_card


class ApiError(Exception):
    """An action refused; @message is meant for the user. @code says what kind (400 bad input, 409 no device...)."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message


# --------------------------------------------------------------------------
# Finding the device
# --------------------------------------------------------------------------

def _windows_roots():
    """Drive roots that could hold the device, without waking empty readers."""
    import ctypes
    kernel32 = ctypes.windll.kernel32
    old = kernel32.SetErrorMode(0x0001)          # SEM_FAILCRITICALERRORS: no "insert a disk" pop-ups
    try:
        mask = kernel32.GetLogicalDrives()
        roots = []
        for i in range(26):
            if mask & (1 << i):
                root = "%s:\\" % chr(ord("A") + i)
                kind = kernel32.GetDriveTypeW(root)
                if kind in (2, 3):               # removable or fixed; never network, CD or RAM disks
                    roots.append(root)
        return roots
    finally:
        kernel32.SetErrorMode(old)


def _unix_roots():
    roots = []
    for base in ("/Volumes", "/media", "/run/media", "/mnt"):
        if not os.path.isdir(base):
            continue
        try:
            for name in sorted(os.listdir(base)):
                p = os.path.join(base, name)
                if os.path.isdir(p):
                    roots.append(p)
                    # /media/<user>/<volume> and /run/media/<user>/<volume>
                    try:
                        for sub in sorted(os.listdir(p)):
                            q = os.path.join(p, sub)
                            if os.path.isdir(q):
                                roots.append(q)
                    except OSError:
                        pass
        except OSError:
            pass
    return roots


def candidate_roots():
    try:
        return _windows_roots() if os.name == "nt" else _unix_roots()
    except Exception:
        return []


def read_text(path, limit=None):
    with open(path, "rb") as f:
        data = f.read() if limit is None else f.read(limit)
    return data.decode("utf-8-sig", errors="replace")


def _read_all(fd, size):
    out = b""
    while len(out) < size:
        chunk = os.read(fd, size - len(out))
        if not chunk:
            break
        out += chunk
    return out


def _read_direct_linux(path, size):
    """O_DIRECT: the read goes to the device. It wants a page-aligned buffer, which an anonymous mmap is."""
    import mmap
    fd = os.open(path, os.O_RDONLY | os.O_DIRECT)
    try:
        buf = mmap.mmap(-1, max(4096, (size + 4095) // 4096 * 4096))
        try:
            got = os.readv(fd, [buf])
            return buf[:min(got, size)]
        finally:
            buf.close()
    finally:
        os.close(fd)


def _read_dropping_cache(path, size):
    """Linux without O_DIRECT (some file systems refuse it): drop the file's cached pages, then read."""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        return _read_all(fd, size)
    finally:
        os.close(fd)


def _read_nocache_mac(path, size):
    import fcntl
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.fcntl(fd, 48, 1)                   # F_NOCACHE
        return _read_all(fd, size)
    finally:
        os.close(fd)


def _read_unbuffered_windows(path, size):
    """FILE_FLAG_NO_BUFFERING: whole sectors into a sector-aligned buffer (VirtualAlloc gives a page)."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.VirtualAlloc.restype = ctypes.c_void_p
    k.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    k.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
    k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                           wintypes.LPVOID]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    n = max(4096, (size + 4095) // 4096 * 4096)  # a multiple of any sector size
    h = k.CreateFileW(path, 0x80000000, 3, None, 3, 0x20000000, None)   # read, share both, open existing, no buffering
    if h is None or h == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW failed")
    buf = None
    try:
        buf = k.VirtualAlloc(None, n, 0x3000, 0x04)     # MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        if not buf:
            raise OSError(ctypes.get_last_error(), "VirtualAlloc failed")
        got = wintypes.DWORD()
        if not k.ReadFile(h, buf, n, ctypes.byref(got), None):
            raise OSError(ctypes.get_last_error(), "ReadFile failed")
        return ctypes.string_at(buf, min(got.value, size))
    finally:
        if buf:
            k.VirtualFree(buf, 0, 0x8000)                # MEM_RELEASE
        k.CloseHandle(h)


def read_uncached(path, size=512):
    """
    Up to @size bytes of @path read from the drive itself, not from the system's cache. The device makes
    STATUS.TXT and LASTCARD.TXT afresh at every read, but the system would keep handing back the copy it read
    first. Falls back to a plain read wherever the uncached one fails (another file system, an old system).
    """
    try:
        system = platform.system()
        if system == "Windows":
            return _read_unbuffered_windows(path, size)
        if system == "Darwin":
            return _read_nocache_mac(path, size)
        if hasattr(os, "O_DIRECT"):
            try:
                return _read_direct_linux(path, size)
            except OSError:
                if not hasattr(os, "posix_fadvise"):
                    raise
                return _read_dropping_cache(path, size)
    except Exception:                            # any trouble: the plain read below
        pass
    with open(path, "rb") as f:
        return f.read(size)


def is_device(root):
    """True when @root holds the three files and STATUS.TXT says what it should."""
    try:
        for name in (STATUS_NAME, ATTEND_NAME, SETTINGS_NAME):
            if not os.path.isfile(os.path.join(root, name)):
                return False
        return read_text(os.path.join(root, STATUS_NAME), 64).lstrip().startswith(STATUS_MAGIC)
    except OSError:
        return False


def find_device(forced=None):
    """The folder of a plugged-in device, or None."""
    if forced:
        return forced if is_device(forced) else None
    for root in candidate_roots():
        if is_device(root):
            return root
    return None


EJECT_BUSY = "The drive is in use by another program: close any window showing it and try again."
EJECT_DENIED = ("This computer did not allow the app to eject the drive. Use Eject in the file manager, or press the "
                "button on the device once.")
EJECT_RETRIES = 4              # udisksctl unmount attempts before other ways are tried ...
EJECT_RETRY_S = 0.5            # ... this far apart (a file indexer lets go quickly)


def _run(args, timeout=30):
    """(worked?, what it printed): a command run without a console window."""
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, creationflags=flags)
    except (OSError, subprocess.SubprocessError) as e:
        return False, str(e)
    return r.returncode == 0, (r.stderr or b"").decode(errors="replace").strip() or \
        (r.stdout or b"").decode(errors="replace").strip()


def eject_reason(output):
    """What to tell the user when the system refused an eject, from what its tool printed."""
    low = (output or "").lower()
    if "busy" in low or "in use" in low:
        return EJECT_BUSY
    if "not authorized" in low or "notauthorized" in low or "not permitted" in low or "permission" in low:
        return EJECT_DENIED
    detail = " ".join((output or "").split())[:160]
    return ("Could not eject the drive%s. Use Eject in the file manager, or press the button on the device once."
            % (" (%s)" % detail if detail else ""))


def _eject_windows(root):
    """Lock, dismount and eject the volume: what Explorer's Eject does, without its localised verb."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    drive = os.path.splitdrive(os.path.abspath(root))[0]
    h = k.CreateFileW("\\\\.\\" + drive, 0xC0000000, 3, None, 3, 0, None)   # read/write, share both, open existing
    if h is None or h == wintypes.HANDLE(-1).value:
        return False
    try:
        done = wintypes.DWORD()

        def ioctl(code, buf=None, size=0):
            return bool(k.DeviceIoControl(h, code, buf, size, None, 0, ctypes.byref(done), None))
        for _ in range(20):                      # FSCTL_LOCK_VOLUME: waits for Explorer to let go
            if ioctl(0x00090018):
                break
            time.sleep(0.25)
        else:
            return False
        if not ioctl(0x00090020):                # FSCTL_DISMOUNT_VOLUME
            return False
        allow = ctypes.c_ubyte(0)
        ioctl(0x002D4804, ctypes.byref(allow), 1)  # IOCTL_STORAGE_MEDIA_REMOVAL: allow
        return ioctl(0x002D4808)                 # IOCTL_STORAGE_EJECT_MEDIA: the SCSI eject the device waits for
    finally:
        k.CloseHandle(h)


def _block_device(root):
    """/dev/... mounted at @root, from /proc/mounts (Linux), or None."""
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) > 1 and parts[1].encode().decode("unicode_escape") == root.rstrip("/"):
                    return parts[0]
    except OSError:
        pass
    return None


def _udisks_drive(dev):
    """The UDisks2 object path of the drive holding @dev (udisksctl info's Drive: line), or None."""
    ok, out = _run(["udisksctl", "info", "-b", dev])
    m = re.search(r"^\s*Drive:\s*'?(/org/freedesktop/UDisks2/drives/[^'\s]+)'?", out, re.M) if ok else None
    return m.group(1) if m else None


def _eject_linux(root):
    """
    (worked?, why not) for Linux. A file manager or indexer showing the drive makes the first unmount fail
    as busy, so: sync, unmount through UDisks with a few retries, else let GVfs eject it (it asks the file
    manager to let go), else a forced (lazy) unmount, which is safe once everything is synced. Then the drive
    is powered off, or, where that is refused, ejected through UDisks2: either way the device gets the SCSI
    eject it waits for, applies SETTINGS.CSV and leaves the bus by itself.
    """
    try:
        os.sync()
    except (AttributeError, OSError):
        pass
    dev = _block_device(root)
    udisks = bool(dev and shutil.which("udisksctl"))
    why = ""
    unmounted = False
    if udisks:
        for i in range(EJECT_RETRIES):
            if i:
                time.sleep(EJECT_RETRY_S)
            unmounted, out = _run(["udisksctl", "unmount", "--no-user-interaction", "-b", dev])
            if unmounted:
                break
            why = out
    if not unmounted and shutil.which("gio"):
        ok, out = _run(["gio", "mount", "--eject", root])
        if ok:
            return True, ""
        why = why or out
    if not unmounted and udisks:
        unmounted, out = _run(["udisksctl", "unmount", "--force", "--no-user-interaction", "-b", dev])
        why = why or out
    if unmounted:
        ok, out = _run(["udisksctl", "power-off", "--no-user-interaction", "-b", dev])
        if ok:
            return True, ""
        drive = _udisks_drive(dev) if shutil.which("gdbus") else None
        if drive:
            ok, out2 = _run(["gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2", "--object-path", drive,
                             "--method", "org.freedesktop.UDisks2.Drive.Eject", "{}"])
            if ok:
                return True, ""
            out = out2 or out
        return False, ("The drive was unmounted, but the system would not eject it (%s). Press the button on the "
                       "device once, or unplug it." % (" ".join(out.split())[:120] or "no reason given"))
    if not udisks and not shutil.which("gio") and shutil.which("eject"):
        ok, out = _run(["eject", root])
        if ok:
            return True, ""
        why = out
    if not dev and not why:
        return False, ("The drive is not mounted where this app can find it. Use Eject in the file manager, or "
                       "press the button on the device once.")
    return False, eject_reason(why)


def eject_drive(root):
    """
    Eject the device's drive, as the system's own Eject does. The device sees it, applies SETTINGS.CSV and
    starts taking attendance with the cable still in. Returns (worked?, why not: a message for the user).
    """
    try:
        system = platform.system()
        if system == "Windows":
            if _eject_windows(root):
                return True, ""
            return False, ("Windows would not eject the drive. Close any window or program using it and try again, "
                           "or press the button on the device once.")
        if system == "Darwin":
            ok, out = _run(["diskutil", "eject", root])
            return ok, ("" if ok else eject_reason(out))
        return _eject_linux(root)
    except (OSError, subprocess.SubprocessError) as e:
        return False, eject_reason(str(e))


def volume_label(root):
    if os.name != "nt":
        return os.path.basename(root.rstrip("/")) or root
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(261)
        if ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 261, None, None, None, None, 0):
            return buf.value
    except Exception:
        pass
    return ""


# --------------------------------------------------------------------------
# The application: a database, and at most one device
# --------------------------------------------------------------------------

class App:
    def __init__(self, db, forced_dir=None, data_dir=None):
        self.db = db
        self.forced_dir = forced_dir
        self.data_dir = data_dir
        self.server = None
        self.lock = threading.RLock()
        self.last_path = None
        self.sig = None
        self.status = None              # the device's STATUS.TXT, parsed
        self.last_sync = {"seq": 0, "new": 0, "read": 0, "skipped": 0, "lectures": 0, "at": ""}
        self.pc_minus_device = None     # seconds the PC clock is ahead of the device's, as last read
        self.last_eject_error = ""      # why the last eject failed, for the user; "" when it worked

    # ---- the device
    def device_dir(self):
        with self.lock:
            # Keep using the last folder while it still checks out, which is quick.
            if self.last_path and is_device(self.last_path):
                return self.last_path
            self.last_path = find_device(self.forced_dir)
            return self.last_path

    @staticmethod
    def _read_lectures(root):
        """LECTURES.CSV parsed, (rows, skipped), or None when this firmware does not write one."""
        path = os.path.join(root, LECTURES_NAME)
        if not os.path.isfile(path):
            return None
        return D.parse_lectures(read_text(path))

    def _sync(self, root, st):
        """Read the taps and lectures off the device into the database. Safe to repeat: known ones are skipped."""
        rows, skipped = D.parse_attend(read_text(os.path.join(root, ATTEND_NAME)))
        new = self.db.add_taps(rows, st["device_id"])
        lectures = self._read_lectures(root)
        added = 0
        if lectures is not None:
            # Every lecture start, so lectures the device started by itself each get their own taps.
            added = self.db.take_device_lectures(lectures[0])
            # And the taps from before the first of them, as the device's L000 file has them.
            first = min((r[0] for r in lectures[0]), default=None)
            if self.db.take_lecture_zero(rows, first):
                added += 1
        elif st["has_lecture"] and st["since"]:
            # Older firmware: only the newest lecture, from STATUS.TXT.
            if self.db.confirm_lecture_start(st["module"], st["lecture"], st["since"]):
                self.db.note_confirmed(st["since"])
            elif self.db.adopt_device_lecture(st["module"], st["lecture"], st["since"]):
                added = 1
        self.last_sync = {"seq": self.last_sync["seq"] + 1, "new": new, "read": len(rows), "skipped": skipped,
                          "lectures": added, "at": time.strftime("%H:%M:%S")}

    def refresh(self, force=False):
        """Look at the device; read its taps if anything changed. Returns the state for the page."""
        with self.lock:
            root = self.device_dir()
            if root:
                try:
                    # Past the cache: the device rewrites STATUS.TXT at every read (the battery, the clock).
                    st = D.parse_status(read_uncached(os.path.join(root, STATUS_NAME), 1024).decode(
                        "utf-8-sig", errors="replace"))
                    stat = os.stat(os.path.join(root, ATTEND_NAME))
                    sig = (root, stat.st_size, int(stat.st_mtime), st["records"], st["last_card"], st["last_tap"],
                           st["module"], st["lecture"], st["since"])
                    if force or sig != self.sig:
                        self._sync(root, st)
                        self.sig = sig
                    self.status = st
                    if st["clock"] is not None:
                        self.pc_minus_device = D.now_ts() - st["clock"]
                except OSError:
                    root = None
                    self.sig = None
            if not root:
                self.sig = None
                self.status = None
            return self.state(root)

    def state(self, root):
        st = self.status if root else None
        out = {"connected": bool(root), "demo": bool(self.forced_dir), "platform": platform.system(),
               "now": time.strftime("%Y-%m-%d %H:%M:%S"), "pdf": R.find_browser() is not None,
               "counts": self.db.counts(), "sync": self.last_sync, "current_lecture": self.db.current_lecture(),
               "db_path": getattr(self.db, "path", ""), "last_sync_db": self.db.last_sync()}
        if root and st:
            out.update({
                "path": root, "label": volume_label(root),
                "device": {"device_id": st["device_id"], "clock": D.fmt_ts(st["clock"]) if st["clock"] is not None else "",
                           "records": st["records"], "last_card": st["last_card"],
                           "last_tap": D.fmt_ts(st["last_tap"]) if st["last_tap"] is not None else "",
                           "module": st["module"], "lecture": st["lecture"], "has_lecture": st["has_lecture"],
                           "since": D.fmt_ts(st["since"]) if st["since"] is not None else "",
                           "pending": st["pending"], "error": st["error"], "note": st["note"],
                           "battery_percent": st.get("battery_percent"), "battery_mv": st.get("battery_mv")},
                "drift_seconds": (-self.pc_minus_device if self.pc_minus_device is not None else None)})
        return out

    def write_settings(self, text):
        root = self.device_dir()
        if not root:
            raise ApiError(409, "The device is not connected. Plug it in with the USB-C cable.")
        data = text.encode("utf-8")
        if len(data) > MAX_SETTINGS_BYTES or b"\x00" in data:
            raise ApiError(413, "That settings file is too large for the device.")
        # Written in place, not via a temporary file and a rename: the device
        # is a tiny FAT volume and this keeps its directory tidy.
        try:
            with open(os.path.join(root, SETTINGS_NAME), "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
        except OSError as e:
            raise ApiError(500, "Could not write to the device: %s" % e)

    def device_offset(self):
        """Seconds to add to this PC's clock to get the device's (0 when unknown)."""
        return -self.pc_minus_device if self.pc_minus_device is not None else 0

    # ---- what the screens ask of the device (both windows call these)
    def import_everything(self):
        """
        Read every tap and lecture start off the device into the database, now. True only when ATTEND.CSV
        held exactly the number of rows STATUS.TXT promises, and every row of it and of LECTURES.CSV (when
        the firmware writes one) was readable: then the device may be cleared, lecture markers and all.
        """
        with self.lock:
            root = self.device_dir()
            if not root:
                return False
            st = D.parse_status(read_text(os.path.join(root, STATUS_NAME), 1024))
            rows, skipped = D.parse_attend(read_text(os.path.join(root, ATTEND_NAME)))
            if not st["ok"] or skipped or len(rows) != st["records"]:
                return False
            try:
                lectures = self._read_lectures(root)
            except OSError:
                return False
            if lectures is not None and lectures[1]:
                return False
            self._sync(root, st)
            self.sig = None                     # read STATUS.TXT afresh on the next poll
            return True

    def eject(self):
        """Eject the drive so the device applies SETTINGS.CSV and starts scanning. True when it worked; when it
        did not, last_eject_error says why."""
        with self.lock:
            root = self.device_dir()
            if not root:
                ok, why = False, "The device is not connected."
            elif self.forced_dir:
                ok, why = False, "This is a folder standing in for the device (the demo), which cannot be ejected."
            else:
                ok, why = eject_drive(root)
            self.last_eject_error = "" if ok else why
            if ok:
                self.last_path = None
                self.sig = None
            return ok

    def start_lecture(self, module, title, sync_clock=True, clear_device=True):
        """
        Tell the device which lecture this is, and record it here. With @clear_device the device's
        records are imported first and the device deletes them when it starts the lecture. The drive
        is then ejected so the lecture starts at once. Returns {lecture, clock_set, cleared, ejected}.
        """
        module = D.clean_device_text(module, D.MODULE_BYTES)
        title = D.clean_device_text(title, D.LECTURE_BYTES)
        if not module:
            raise ApiError(400, "Choose or enter a module")
        if not title:
            raise ApiError(400, "Enter a lecture name")
        sync = sync_clock is not False
        st = self.status or {}
        now = D.now_ts()
        clear = bool(clear_device) and self.import_everything()
        text = D.build_settings(now=now if sync else None, module=module, lecture=title, new_session=True,
                                device_id=st.get("device_id", 0), clear_log=clear)
        self.write_settings(text)                # first: if the device cannot be written, nothing is recorded here
        start = now if sync else now + int(self.device_offset())
        lecture = self.db.start_lecture(module, title, start)
        ejected = self.eject()
        return {"lecture": lecture, "clock_set": sync, "cleared": clear, "ejected": ejected,
                "eject_error": self.last_eject_error}

    def set_clock(self):
        """Set the device clock to this computer's time."""
        st = self.status or {}
        self.write_settings(D.build_settings(now=D.now_ts(), device_id=st.get("device_id", 0)))

    def set_clock_and_eject(self):
        """set_clock(), then eject so the device takes it at once. Returns {ejected, eject_error}."""
        self.set_clock()
        ejected = self.eject()
        return {"ejected": ejected, "eject_error": self.last_eject_error}

    def clear_device(self):
        """
        Empty the device's flash log: first every tap and lecture is copied into the database, and only when
        all of them were read is the device told to erase its copy (#CLEARLOG). The drive is then ejected so
        the device does it at once. Students and everything in the database are kept. Returns {ejected}.
        """
        if not self.device_dir():
            raise ApiError(409, "The device is not connected. Plug it in with the USB-C cable.")
        if not self.import_everything():
            raise ApiError(409, "Not every record could be read from the device, so nothing was erased. "
                                "Unplug it, plug it in again and try once more.")
        st = self.status or {}
        self.write_settings(D.build_settings(device_id=st.get("device_id", 0), clear_log=True))
        ejected = self.eject()
        return {"ejected": ejected, "eject_error": self.last_eject_error}

    def erase_device(self):
        """
        Erase the device's flash log WITHOUT copying it here first (#CLEARLOG): every tap and lecture not yet in
        the database is lost. For a log that cannot be read, which clear_device() refuses to erase. The device
        keeps its number and clock, and numbers its lectures from 1 again. The drive is then ejected so the
        device does it at once. Returns {ejected}.
        """
        if not self.device_dir():
            raise ApiError(409, "The device is not connected. Plug it in with the USB-C cable.")
        st = self.status or {}
        self.write_settings(D.build_settings(device_id=st.get("device_id", 0), clear_log=True))
        self.sig = None
        ejected = self.eject()
        return {"ejected": ejected, "eject_error": self.last_eject_error}

    def clear_records(self, also_device=False):
        """
        Delete every lecture and tap from this computer's database, keeping the students, modules and
        departments, after saving a copy of the database (snapshot_backup()). A fresh start: the next read
        of the device takes in whatever it still holds, lectures and all. With @also_device, and the device
        plugged in, the device is then told to erase its records without saving them (erase_device()), so
        they do not come back. Returns {backup, lectures, taps, device: erase_device()'s result or None,
        device_error: why the device could not be erased, or ""}.
        """
        with self.lock:
            backup = snapshot_backup(self.db, self.data_dir, "before-clear")
            gone = self.db.clear_attendance()
            out = {"backup": backup, "lectures": gone["lectures"], "taps": gone["taps"], "device": None,
                   "device_error": ""}
            if also_device and self.device_dir():
                sig = self.sig
                try:
                    out["device"] = self.erase_device()
                except ApiError as e:
                    out["device_error"] = e.message
                if out["device"] and not out["device"]["ejected"]:
                    # Not ejected: the device erases on its button or unplugging. Until it does, do not read
                    # back the records it is about to erase (an explicit Read still does).
                    self.sig = sig
                    return out
            self.sig = None                     # read the device afresh on the next poll
            return out

    def last_card(self):
        """
        The card last tapped on the plugged-in device, from LASTCARD.TXT read past the system's cache:
        {"taps", "card_id", "uid"} (see attendance_db.parse_last_card). None when no device is plugged in, or
        its firmware does not write the file. Drive I/O: call it from a worker thread.
        """
        root = self.device_dir()
        if not root:
            return None
        try:
            data = read_uncached(os.path.join(root, LASTCARD_NAME), 512)
        except OSError:
            return None
        return D.parse_last_card(data.decode("utf-8", errors="replace"))

    def set_device_id(self, value):
        dev = _int(value, "device number")
        if dev < 0 or dev >= 0xFFFFFFFF:
            raise ApiError(400, "The device number must be between 0 and 4294967294")
        self.write_settings(D.build_settings(device_id=dev))


def snapshot_backup(db, data_dir, why="copy"):
    """A copy of the database now, in the backups folder, named with the date and time and @why (so the daily
    copies are neither overwritten nor counted by rotate_backup()). Returns its path. Raises OSError."""
    folder = os.path.join(data_dir or os.path.dirname(os.path.abspath(db.path)), "backups")
    os.makedirs(folder, exist_ok=True)
    base = "attendance-%s-%s" % (time.strftime("%Y%m%d-%H%M%S"), _slug(why))
    path, n = os.path.join(folder, base + ".db"), 1
    while os.path.exists(path):
        n += 1
        path = os.path.join(folder, "%s-%d.db" % (base, n))
    data = db.backup_bytes()
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    return path


def rotate_backup(db, data_dir):
    """Keep a dated copy of the database, one a day, the last BACKUPS_KEPT of them."""
    if not data_dir:
        return None
    folder = os.path.join(data_dir, "backups")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "attendance-%s.db" % datetime.date.today().strftime("%Y%m%d"))
    if not os.path.exists(path) and db.counts()["taps"] + db.counts()["students"] > 0:
        with open(path, "wb") as f:
            f.write(db.backup_bytes())
    files = sorted(f for f in os.listdir(folder) if re.fullmatch(r"attendance-\d{8}\.db", f))
    for old in files[:-BACKUPS_KEPT]:
        try:
            os.remove(os.path.join(folder, old))
        except OSError:
            pass
    return path


# --------------------------------------------------------------------------
# Actions and exports the windows share
# --------------------------------------------------------------------------

CSV_TYPE = "text/csv; charset=utf-8"
HTML_TYPE = "text/html; charset=utf-8"
PDF_TYPE = "application/pdf"


def _int(v, what="number"):
    try:
        return int(v)
    except (TypeError, ValueError):
        raise ApiError(400, "That is not a valid %s" % what)


def opt_ts(v):
    """A typed date or date and time as a naive epoch; empty means None."""
    v = (v or "").strip() if isinstance(v, str) else v
    return D.parse_ts(v) if v else None


def _slug(s):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", str(s)).strip("_") or "attendance"


def import_taps(db, text):
    """Taps from a saved ATTEND.CSV, when the device itself is not to hand. Returns {read, new, skipped}."""
    if not isinstance(text, str):
        raise ApiError(400, "There is no text to import.")
    rows, skipped = D.parse_attend(text)
    if not rows:
        raise ApiError(400, "No taps found in that file. It should be the ATTEND.CSV from the device.")
    return {"read": len(rows), "new": db.add_taps(rows, 0), "skipped": skipped}


def edit_lecture(db, lecture_id, fields):
    """Change a lecture. @fields holds any of module, title, start, end (dates as typed; an empty end means running)."""
    kw = {}
    if "module" in fields:
        kw["module"] = fields["module"]
    if "title" in fields:
        kw["title"] = fields["title"]
    if "start" in fields:
        kw["start_ts"] = opt_ts(fields["start"])
    if "end" in fields:
        kw["end_ts"] = opt_ts(fields["end"])
    return db.update_lecture(lecture_id, **kw)


def save_module(db, old, code, title="", department=""):
    """Add a module, or edit one (renaming it first when @old differs from @code), as the Modules screen does."""
    if old and old != str(code or "").strip():
        db.rename_module(old, code)
    return db.upsert_module(code, title, department)


def _pdf_bytes(page, table=None):
    """A rich report printed by the PC's browser; without one, @table() (title, subtitle, headers, rows) is
    written as a plain PDF instead (report_pages.table_pdf), so a PDF never fails for the want of a browser."""
    try:
        return R.render_pdf(page)
    except RuntimeError as e:
        if table is None or R.find_browser():          # a browser that failed: say so
            raise ApiError(501, str(e))
        return R.table_pdf(*table())


def _export(kind, csv_text, page, base, table=None):
    """(data, content type, file name) for kind csv, html or pdf. @page and @table are called only when needed."""
    if kind == "csv":
        return ("\ufeff" + csv_text()).encode("utf-8"), CSV_TYPE, base + ".csv"
    if kind == "html":
        return page().encode("utf-8"), HTML_TYPE, base + ".html"
    if kind == "pdf":
        return _pdf_bytes(page(), table), PDF_TYPE, base + ".pdf"
    raise ValueError(kind)


def lecture_export(db, lecture_id, kind):
    att = db.lecture_attendance(lecture_id)
    l = att["lecture"]

    def table():
        rows = [[p["name"], p["student_no"], D.fmt_card(p["card_id"]), "Present", p["time"]] for p in att["present"]]
        rows += [[p["name"], p["student_no"], D.fmt_card(p["card_id"]), "Present (not enrolled)", p["time"]]
                 for p in att["present_other"]]
        rows += [[a["name"], a["student_no"], D.fmt_card(a["card_id"]), "Absent", ""] for a in att["absent"]]
        rows += [["(card not registered)", "", D.fmt_card(u["card_id"]), "", u["time"]] for u in att["unregistered"]]
        return ("%s %s" % (l["module_code"], l["title"]), "%s, %s to %s" % (l["date"], l["start_text"][11:16],
                l["end_text"][11:16]), ["Name", "Student no", "Card", "Status", "Time"], rows)
    return _export(kind, lambda: D.Database.lecture_csv(att), lambda: R.lecture_page(att),
                   "%s_%s_%s" % (_slug(l["date"]), _slug(l["module_code"]), _slug(l["title"])), table)


def module_export(db, code, kind, date_from=None, date_to=None):
    rep = db.module_report(code, date_from or None, date_to or None)

    def table():
        return ("%s %s" % (rep["module"]["code"], rep["module"]["title"]), "Attendance report",
                ["Name", "Student no", "Card", "Present", "%"],
                [[s["name"], s["student_no"], D.fmt_card(s["card_id"]), "%d/%d" % (s["count"], len(rep["lectures"])),
                  s["percent"]] for s in rep["students"]])
    return _export(kind, lambda: D.Database.module_csv(rep), lambda: R.module_page(rep), "%s_attendance" % _slug(code),
                   table)


def student_export(db, card_id, kind):
    rep = db.student_report(card_id)
    st = rep["student"]

    def table():
        return (st["name"], "Card %s, student no %s" % (D.fmt_card(st["card_id"]), st["student_no"] or "-"),
                ["Module", "Date", "Lecture", "Attended", "Time"],
                [[m["code"], l["date"], l["title"], "Yes" if l["attended"] else "No", l["time"]]
                 for m in rep["modules"] for l in m["lectures"]])
    return _export(kind, lambda: D.Database.student_csv(rep), lambda: R.student_page(rep),
                   "%s_attendance" % _slug(st["name"]), table)


def students_table(db):
    """The student list as (title, subtitle, headers, rows), as the Students page shows it."""
    students = sorted(db.list_students(), key=lambda s: (s["student_no"] or "\uffff", s["name"].lower()))
    dept = any(s["department"] for s in students)        # the column only when someone has one
    return ("Students", "%d registered" % len(students),
            ["Index No", "Name", "Card"] + (["Department"] if dept else []) + ["Last tap"],
            [[s["student_no"], s["name"], D.fmt_card(s["card_id"])] + ([s["department"]] if dept else []) +
             [s["last_tap_text"]] for s in students])


def taps_table(db, ts_from=None, ts_to=None):
    """Every tap, oldest first, as (title, subtitle, headers, rows): who it was, when unregistered cards blank."""
    taps = list(reversed(db.list_taps(ts_from, ts_to, limit=1000000)))
    span = " from %s to %s" % (taps[0]["time"][:10], taps[-1]["time"][:10]) if taps else ""
    return ("All attendance records", "%d taps%s" % (len(taps), span), ["Date", "Time", "Card", "Index No", "Name"],
            [[t["time"][:10], t["time"][11:], D.fmt_card(t["card_id"]), t["student_no"] or "",
              t["name"] or "(card not registered)"] for t in taps])


def students_export(db, kind="csv"):
    """The student list: kind csv (every column, for importing again) or pdf (to print)."""
    if kind == "pdf":
        return R.table_pdf(*students_table(db)), PDF_TYPE, "students.pdf"
    return ("\ufeff" + db.students_csv()).encode("utf-8"), CSV_TYPE, "students.csv"


def taps_export(db, ts_from=None, ts_to=None, kind="csv"):
    """Every tap (all attendance records): kind csv or pdf."""
    if kind == "pdf":
        return R.table_pdf(*taps_table(db, ts_from, ts_to)), PDF_TYPE, "taps.pdf"
    return ("\ufeff" + db.taps_csv(ts_from, ts_to)).encode("utf-8"), CSV_TYPE, "taps.csv"


def backup_export(db):
    return db.backup_bytes(), "application/octet-stream", "attendance-%s.db" % time.strftime("%Y%m%d-%H%M")


# --------------------------------------------------------------------------
# Demo data: a believable department, and a device that holds its taps
# --------------------------------------------------------------------------

def make_demo(data_dir, device_dir):
    """Fill @data_dir with a database and @device_dir with the device's files."""
    import random
    rnd = random.Random(11)
    db = D.Database(os.path.join(data_dir, "attendance.db"))
    first = ["Amal", "Nimali", "Kasun", "Dilani", "Ruwan", "Sanduni", "Tharindu", "Ishara", "Chamara", "Hasini",
             "Pradeep", "Mihiri", "Lahiru", "Nadeesha", "Gayan", "Oshadi", "Supun", "Kavindi", "Dinesh", "Yashodha",
             "Isuru", "Sachini", "Buddhika", "Anushka", "Malith", "Thilini", "Janaka", "Piumi", "Harsha", "Dulani"]
    last = ["Perera", "Silva", "Fernando", "Jayasinghe", "Bandara", "Wickramasinghe", "Gunawardena", "Rajapaksa"]
    db.upsert_module("EN2090", "Circuits and Systems", "Electrical Engineering")
    db.upsert_module("MA1010", "Calculus", "Mathematics")
    db.upsert_module("CS1010", "Programming Fundamentals", "Computer Science")
    people = []
    for i, f in enumerate(first):
        card = 1000 + 7 * i
        dept = "Electrical Engineering" if i % 3 != 2 else "Computer Science"
        mods = ["MA1010"] + (["EN2090"] if dept.startswith("Elec") else ["CS1010"])
        db.upsert_student(card, "%s %s" % (f, last[i % len(last)]), "EN/%02d/%03d" % (20 + i % 3, 100 + i), dept, mods)
        people.append((card, mods))
    # Two cards nobody has registered yet: the "enrol me" demo.
    new_cards = [424242, 535353]

    today = datetime.date.today()
    day = lambda n: datetime.datetime.combine(today - datetime.timedelta(days=n), datetime.time(9, 0, 0))
    plan = [("EN2090", "Circuits Lecture 1", 21, 0.93), ("EN2090", "Circuits Lecture 2", 14, 0.85),
            ("EN2090", "Circuits Lecture 3", 7, 0.74), ("MA1010", "Calculus Lecture 1", 20, 0.9),
            ("MA1010", "Calculus Lecture 2", 13, 0.8), ("CS1010", "Intro to Python", 15, 0.88)]
    taps, lectures = [], []
    for module, title, ago, rate in plan:
        start = day(ago)
        lec = db.create_lecture(module, title, D.to_ts(start), D.to_ts(start) + 3600)
        lectures.append((D.to_ts(start), module, title))
        for card, mods in people:
            if module in mods and rnd.random() < rate:
                taps.append((D.to_ts(start) + rnd.randint(2, 480), card))
    # A lab the lecturer started on the device itself (a long press), named after the lecture before it.
    # Only the device knows it: the first read finds it in LECTURES.CSV and files the lab's taps under it.
    lab, lab_rnd = D.to_ts(day(15)) + 2 * 3600, random.Random(29)     # its own numbers: the rest stays as it was
    lectures.append((lab, "CS1010", "Intro to Python 2"))
    for card, mods in people:
        if "CS1010" in mods and lab_rnd.random() < 0.8:
            taps.append((lab + lab_rnd.randint(5, 600), card))
    # One lecture today, still running, with a new card in it.
    now = datetime.datetime.now().replace(microsecond=0)
    start = now - datetime.timedelta(minutes=40)
    cur = db.start_lecture("EN2090", "Circuits Lecture 4", D.to_ts(start))
    lectures.append((D.to_ts(start), "EN2090", "Circuits Lecture 4"))
    for card, mods in people:
        if "EN2090" in mods and rnd.random() < 0.7:
            taps.append((D.to_ts(start) + rnd.randint(30, 1500), card))
    taps.append((D.to_ts(start) + 900, new_cards[0]))
    taps.append((D.to_ts(start) + 1200, new_cards[1]))
    # Some taps with no lecture running: attendance taken without starting one.
    loose = datetime.datetime.combine(today - datetime.timedelta(days=3), datetime.time(15, 30, 0))
    for card, mods in people[:9]:
        taps.append((D.to_ts(loose) + rnd.randint(0, 300), card))
    taps.sort()
    # The database starts with no taps; the device log holds them all, and the first read brings them in.
    db.close()
    del cur

    def pad(s, n):
        return s.ljust(n)[:n]

    attend = pad("DATE,TIME,CARD_ID", 30) + "\r\n"
    for ts, card in taps:
        attend += "%s,%s,%010d\r\n" % (D.fmt_ts(ts)[:10], D.fmt_ts(ts)[11:], card)
    # Every lecture start in the log, in the same fixed-width form: 128-byte rows, four to a sector.
    lectures_csv = pad("DATE,TIME,MODULE,LECTURE", 126) + "\r\n"
    for ts, module, title in sorted(lectures):
        lectures_csv += pad("%s,%s,%s,%s" % (D.fmt_ts(ts)[:10], D.fmt_ts(ts)[11:], module, title), 126) + "\r\n"
    last_ts, last_card = taps[-1]
    status = ("ATTENDANCE LOGGER\r\nDevice ID    : 0012648430\r\nClock        : %s\r\nBattery      : 87 %% (3950 mV)\r\n"
              "Attendance   : %d records in ATTEND.CSV\r\n"
              "Last tap     : %010d at %s\r\nLecture      : EN2090 / Circuits Lecture 4 (since %s)\r\nSETTINGS.CSV : unchanged\r\n"
              % (now.strftime("%Y-%m-%d %H:%M:%S"), len(taps), last_card, D.fmt_ts(last_ts), start.strftime("%Y-%m-%d %H:%M:%S")))
    status = status.ljust(510) + "\r\n"
    settings = ("# Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.\r\n"
                "#TIME,%s\r\n#MODULE,EN2090\r\n#LECTURE,Circuits Lecture 4\r\n#DEVICE,0012648430\r\n" % now.strftime("%Y-%m-%d %H:%M:%S"))
    # Nothing tapped since it was plugged in (a demo device never sees a card, so tapping waits forever).
    lastcard = last_card_text(0)
    for name, text in ((ATTEND_NAME, attend), (LECTURES_NAME, lectures_csv), (STATUS_NAME, status), (SETTINGS_NAME, settings),
                       (LASTCARD_NAME, lastcard)):
        with open(os.path.join(device_dir, name), "w", newline="", encoding="utf-8") as f:
            f.write(text)
    # The LECTURES folder: one CSV per lecture (taps before the first in L000). The app does not read it.
    folder = os.path.join(device_dir, "LECTURES")
    os.makedirs(folder, exist_ok=True)
    starts = sorted(ts for ts, _, _ in lectures)
    for n, ts in enumerate([None] + starts):
        end = starts[n] if n < len(starts) else None
        rows = [(t, c) for t, c in taps if (ts is None or t >= ts) and (end is None or t < end)]
        if ts is None and not rows:
            continue
        name = "L%03d_%s.csv" % (n, D.from_ts(ts if ts is not None else (rows[0][0] if rows else 0)).strftime("%Y-%m-%d_%H-%M"))
        with open(os.path.join(folder, name), "w", newline="", encoding="utf-8") as f:
            f.write(pad("DATE,TIME,CARD_ID", 30) + "\r\n" + "".join(
                "%s,%s,%010d\r\n" % (D.fmt_ts(t)[:10], D.fmt_ts(t)[11:], c) for t, c in rows))


def last_card_text(taps, card_id=None, uid=None):
    """LASTCARD.TXT as the device writes it: four lines, space padded to 510 bytes, then CRLF."""
    text = "LAST CARD\r\nTaps    : %d\r\nCard ID : %s\r\nUID     : %s\r\n" % (
        taps, "%010d" % card_id if card_id else "none yet", uid if card_id and uid else "-")
    return text.ljust(510) + "\r\n"


# --------------------------------------------------------------------------
# Starting up
# --------------------------------------------------------------------------

def default_data_dir():
    return os.path.join(os.path.expanduser("~"), "AttendanceLogger")


def main(argv=None):
    """Opens the window (attendance_qt.py, which needs PySide6)."""
    import attendance_qt
    return attendance_qt.main(argv)


if __name__ == "__main__":
    sys.exit(main())
