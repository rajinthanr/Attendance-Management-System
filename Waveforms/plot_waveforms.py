#!/usr/bin/env python3
"""Browse the CSV files a Tektronix TBS1000C saves to its USB stick (TEKnnnnn.CSV).

Opens a window with a drop-down of every CSV in this folder (or the folder
given); pick one to plot it. CH1 is drawn in yellow and CH2 in blue, on a dark
scope-like background. A file may hold one channel or both (TIME,CH1,CH2).

    python3 plot_waveforms.py                  # browse the CSVs next to this script
    python3 plot_waveforms.py TEK00001.CSV     # open on that file
    python3 plot_waveforms.py --shunt 1.0      # show current through a 1 ohm shunt
    python3 plot_waveforms.py --save           # no window: write TEKnnnnn.png next to each CSV

Needs numpy, matplotlib and tkinter (python3-tk on Debian/Ubuntu).
"""
import argparse
import sys
from pathlib import Path

try:
    import numpy as np
    import matplotlib
    from matplotlib.figure import Figure
except ImportError:
    sys.exit("needs numpy and matplotlib: pip install numpy matplotlib")

COLOURS = {"CH1": "#f5d000", "CH2": "#2f8cff", "CH3": "#ff4fa0", "CH4": "#3fd65a"}
HERE = Path(__file__).resolve().parent


def read_tek_csv(path):
    """Return (header dict, time array, {channel: values})."""
    header = {}
    with open(path, newline="") as f:
        for line in f:
            cells = [c.strip() for c in line.strip().split(",")]
            if cells[0].upper() == "TIME":
                names = [c.upper() for c in cells[1:]]
                break
            if len(cells) >= 2 and cells[0]:
                header[cells[0]] = cells[1]
        else:
            raise ValueError(f"{path.name}: no TIME column header")
        data = np.loadtxt(f, delimiter=",", ndmin=2)
    return header, data[:, 0], {n: data[:, i + 1] for i, n in enumerate(names)}


def time_scale(t):
    span = float(np.max(np.abs(t))) if t.size else 0.0
    for factor, unit in ((1, "s"), (1e3, "ms"), (1e6, "µs"), (1e9, "ns")):
        if span * factor >= 1:
            return factor, unit
    return 1e9, "ns"


def plot_file(ax, path, shunt):
    header, t, channels = read_tek_csv(path)
    factor, tunit = time_scale(t)
    yunit = header.get("Vertical Units", "V")
    if shunt:
        yfactor, yunit = 1e3 / shunt, "mA"
    else:
        yfactor = 1.0
    for name, v in channels.items():
        y = v * yfactor
        label = f"{name}  mean {y.mean():.3g}  min {y.min():.3g}  max {y.max():.3g} {yunit}"
        ax.plot(t * factor, y, color=COLOURS.get(name, "white"), lw=1.0, label=label)

    ax.set_facecolor("black")
    ax.grid(True, color="#444", lw=0.5, ls=":")
    ax.axhline(0, color="#777", lw=0.6)
    ax.axvline(0, color="#777", lw=0.6)   # trigger point
    ax.set_xlim(t[0] * factor, t[-1] * factor)
    ax.set_xlabel(f"time ({tunit})")
    ax.set_ylabel("current (mA)" if shunt else f"voltage ({yunit})")
    info = []
    if "Horizontal Scale" in header:
        info.append(f"{float(header['Horizontal Scale']) * factor:g} {tunit}/div")
    if "Vertical Scale" in header:
        info.append(f"{float(header['Vertical Scale']):g} V/div")
    if "Sample Interval" in header:
        info.append(f"{float(header['Sample Interval']) * 1e6:g} µs/pt")
    ax.set_title(f"{path.name}   " + ", ".join(info), loc="left", fontsize=10)
    ax.legend(loc="upper right", fontsize=8, facecolor="#222", edgecolor="#555",
              labelcolor="white")


def list_csvs(folder):
    return sorted(folder.glob("*.[cC][sS][vV]"))


def save_all(paths, shunt):
    matplotlib.use("Agg")
    for p in paths:
        fig = Figure(figsize=(11, 4.5), layout="constrained")
        plot_file(fig.add_subplot(), p, shunt)
        out = p.with_suffix(".png")
        fig.savefig(out, dpi=150)
        print("wrote", out)


def browse(folder, start, shunt):
    try:
        import tkinter as tk
        from tkinter import ttk, filedialog
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
    except ImportError:
        sys.exit("the window needs tkinter: sudo apt install python3-tk (or use --save)")

    root = tk.Tk()
    root.title("Waveforms")
    root.geometry("1150x600")

    bar = ttk.Frame(root, padding=6)
    bar.pack(side=tk.TOP, fill=tk.X)
    ttk.Label(bar, text="Waveform:").pack(side=tk.LEFT)
    choice = tk.StringVar()
    combo = ttk.Combobox(bar, textvariable=choice, state="readonly", width=40)
    combo.pack(side=tk.LEFT, padx=(4, 12))
    ttk.Label(bar, text="Shunt (Ω, blank = volts):").pack(side=tk.LEFT)
    shunt_var = tk.StringVar(value=f"{shunt:g}" if shunt else "")
    shunt_entry = ttk.Entry(bar, textvariable=shunt_var, width=8)
    shunt_entry.pack(side=tk.LEFT, padx=(4, 12))
    status = ttk.Label(bar, foreground="#b00")
    status.pack(side=tk.RIGHT)

    fig = Figure(figsize=(6, 3), layout="constrained")   # grows to fill the window
    canvas = FigureCanvasTkAgg(fig, master=root)
    NavigationToolbar2Tk(canvas, root).update()
    canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    files = {}

    def refresh(select=None):
        files.clear()
        files.update({p.name: p for p in list_csvs(folder)})
        combo["values"] = list(files)
        if select in files:
            choice.set(select)
        elif choice.get() not in files:
            choice.set(next(iter(files), ""))
        draw()

    def draw(*_):
        fig.clear()
        status.config(text="")
        path = files.get(choice.get())
        if path is None:
            status.config(text=f"no CSV files in {folder}")
        else:
            try:
                ohms = float(shunt_var.get()) if shunt_var.get().strip() else None
                if ohms is not None and ohms <= 0:
                    raise ValueError("shunt must be above 0 Ω")
                plot_file(fig.add_subplot(), path, ohms)
            except (ValueError, OSError) as e:
                fig.clear()
                status.config(text=str(e))
        canvas.draw_idle()

    def step(delta):
        names = list(files)
        if names and choice.get() in names:
            choice.set(names[(names.index(choice.get()) + delta) % len(names)])
            draw()

    def open_folder():
        nonlocal folder
        picked = filedialog.askdirectory(initialdir=folder, title="Waveform folder")
        if picked:
            folder = Path(picked)
            refresh()

    ttk.Button(bar, text="◀", width=3, command=lambda: step(-1)).pack(side=tk.LEFT)
    ttk.Button(bar, text="▶", width=3, command=lambda: step(1)).pack(side=tk.LEFT, padx=(2, 12))
    ttk.Button(bar, text="Refresh", command=lambda: refresh()).pack(side=tk.LEFT)
    ttk.Button(bar, text="Folder…", command=open_folder).pack(side=tk.LEFT, padx=4)

    combo.bind("<<ComboboxSelected>>", draw)
    shunt_entry.bind("<Return>", draw)
    shunt_entry.bind("<FocusOut>", draw)
    root.bind("<Left>", lambda e: step(-1) if e.widget is not shunt_entry else None)
    root.bind("<Right>", lambda e: step(1) if e.widget is not shunt_entry else None)

    refresh(start)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("path", nargs="?", type=Path, default=HERE,
                    help="a CSV to open on, or a folder of them (default: this script's folder)")
    ap.add_argument("--shunt", type=float, metavar="OHMS",
                    help="divide by this shunt resistance and plot milliamps")
    ap.add_argument("--save", action="store_true",
                    help="no window: save a PNG next to each CSV in the folder (or the one file)")
    args = ap.parse_args()

    folder, start = (args.path, None) if args.path.is_dir() else (args.path.parent, args.path.name)
    if args.save:
        paths = list_csvs(folder) if start is None else [args.path]
        if not paths:
            sys.exit(f"no CSV files in {folder}")
        save_all(paths, args.shunt)
    else:
        browse(folder.resolve(), start, args.shunt)


if __name__ == "__main__":
    main()
