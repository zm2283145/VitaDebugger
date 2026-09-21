#!/usr/bin/env python3
"""Tk desktop viewer and receiver for VitaProfiler captures."""

from __future__ import annotations

import concurrent.futures
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Callable

import vitaprofiler_desktop as desktop
import vitaprofiler_trace as trace


DISPLAY_ROW_LIMIT = 3000


def _put_latest(target: queue.Queue[object], value: object) -> None:
    try:
        target.put_nowait(value)
    except queue.Full:
        try:
            target.get_nowait()
        except queue.Empty:
            pass
        target.put_nowait(value)


def _discard_pending(target: queue.Queue[object]) -> None:
    while True:
        try:
            target.get_nowait()
        except queue.Empty:
            return


class ProfilerApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.controller = desktop.ProfilerController()
        self.loaded: desktop.LoadedCapture | None = None
        self.model: desktop.ProfilerViewModel | None = None
        self.executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="vitaprofiler")
        self.cancel_event = threading.Event()
        self.status_messages: queue.Queue[str] = queue.Queue(maxsize=1)
        self.live_updates: queue.Queue[
            tuple[desktop.LoadedCapture, desktop.TableRows]
        ] = queue.Queue(maxsize=1)
        self.busy = False
        self.cancellable = False
        self.row_objects: dict[tuple[str, str], object] = {}

        self.status = tk.StringVar(value="Open a .vptrace capture or start the receiver.")
        self.filter_text = tk.StringVar()
        self.thread_filter = tk.StringVar(value="All threads")
        self.bind_address = tk.StringVar(value="0.0.0.0")
        self.port = tk.StringVar(value=str(trace.DEFAULT_PORT))
        self.source_address = tk.StringVar()

        self._build_window()
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _build_window(self) -> None:
        self.root.title("VitaProfiler Desktop")
        self.root.geometry("1200x780")
        self.root.minsize(900, 600)

        actions = ttk.Frame(self.root, padding=(8, 8, 8, 4))
        actions.pack(fill=tk.X)
        self.open_button = ttk.Button(actions, text="Open capture...",
                                      command=self._choose_capture)
        self.open_button.pack(side=tk.LEFT)
        self.receive_button = ttk.Button(actions, text="Start receiver...",
                                         command=self._choose_receive_output)
        self.receive_button.pack(side=tk.LEFT, padx=(6, 0))
        self.cancel_button = ttk.Button(actions, text="Cancel",
                                        command=self._cancel, state=tk.DISABLED)
        self.cancel_button.pack(side=tk.LEFT, padx=(6, 18))
        self.json_button = ttk.Button(actions, text="Export decoded JSON...",
                                      command=lambda: self._choose_export(False),
                                      state=tk.DISABLED)
        self.json_button.pack(side=tk.LEFT)
        self.perfetto_button = ttk.Button(
            actions, text="Export Perfetto JSON...",
            command=lambda: self._choose_export(True), state=tk.DISABLED)
        self.perfetto_button.pack(side=tk.LEFT, padx=(6, 0))

        receiver = ttk.LabelFrame(self.root, text="TCP receiver", padding=8)
        receiver.pack(fill=tk.X, padx=8, pady=4)
        ttk.Label(receiver, text="Bind").grid(row=0, column=0, sticky=tk.W)
        ttk.Entry(receiver, textvariable=self.bind_address, width=18).grid(
            row=0, column=1, padx=(4, 16), sticky=tk.W)
        ttk.Label(receiver, text="Port").grid(row=0, column=2, sticky=tk.W)
        ttk.Entry(receiver, textvariable=self.port, width=8).grid(
            row=0, column=3, padx=(4, 16), sticky=tk.W)
        ttk.Label(receiver, text="Allowed Vita IPv4 (optional)").grid(
            row=0, column=4, sticky=tk.W)
        ttk.Entry(receiver, textvariable=self.source_address, width=20).grid(
            row=0, column=5, padx=(4, 0), sticky=tk.W)

        filters = ttk.Frame(self.root, padding=(8, 4))
        filters.pack(fill=tk.X)
        ttk.Label(filters, text="Filter").pack(side=tk.LEFT)
        filter_entry = ttk.Entry(filters, textvariable=self.filter_text)
        filter_entry.pack(side=tk.LEFT, padx=(4, 14), fill=tk.X, expand=True)
        filter_entry.bind("<Return>", lambda _event: self._refresh_tables())
        ttk.Label(filters, text="Thread").pack(side=tk.LEFT)
        self.thread_combo = ttk.Combobox(
            filters, textvariable=self.thread_filter, state="readonly",
            values=("All threads",), width=18)
        self.thread_combo.pack(side=tk.LEFT, padx=(4, 6))
        self.thread_combo.bind("<<ComboboxSelected>>",
                               lambda _event: self._refresh_tables())
        ttk.Button(filters, text="Apply", command=self._refresh_tables).pack(
            side=tk.LEFT)
        ttk.Button(filters, text="Clear", command=self._clear_filter).pack(
            side=tk.LEFT, padx=(6, 0))

        body = ttk.Panedwindow(self.root, orient=tk.VERTICAL)
        body.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        self.notebook = ttk.Notebook(body)
        body.add(self.notebook, weight=5)

        overview = ttk.Frame(self.notebook, padding=8)
        overview.columnconfigure(0, weight=1)
        overview.columnconfigure(1, weight=1)
        overview.rowconfigure(1, weight=1)
        ttk.Label(overview, text="Capture metadata").grid(
            row=0, column=0, sticky=tk.W)
        ttk.Label(overview, text="Loss and integrity").grid(
            row=0, column=1, sticky=tk.W, padx=(12, 0))
        self.metadata_tree = self._key_value_tree(overview)
        self.metadata_tree.grid(row=1, column=0, sticky=tk.NSEW)
        self.loss_tree = self._key_value_tree(overview)
        self.loss_tree.grid(row=1, column=1, sticky=tk.NSEW, padx=(12, 0))
        self.notebook.add(overview, text="Overview")

        self.frame_tree = self._add_table(
            "Frames",
            ("index", "time", "duration", "fps", "name", "thread",
             "generation", "sequence", "flags"),
            (70, 120, 100, 80, 240, 110, 90, 90, 180))
        self.timeline_tree = self._add_table(
            "Timeline",
            ("kind", "begin", "duration", "name", "thread",
             "generation", "flags"),
            (80, 120, 100, 300, 110, 90, 180))
        self.zone_tree = self._add_table(
            "Zones",
            ("name", "thread", "begin", "duration", "correlation", "flags"),
            (260, 110, 120, 100, 100, 180))
        self.counter_tree = self._add_table(
            "Counters",
            ("index", "time", "name", "value", "type", "thread",
             "generation", "flags"),
            (70, 120, 260, 120, 130, 110, 90, 180))
        self.event_tree = self._add_table(
            "Events",
            ("index", "time", "type", "name", "value", "thread",
             "correlation", "flags"),
            (70, 120, 130, 250, 110, 110, 100, 180))

        details_frame = ttk.LabelFrame(body, text="Selection", padding=4)
        self.details_tree = self._key_value_tree(details_frame)
        self.details_tree.pack(fill=tk.BOTH, expand=True)
        body.add(details_frame, weight=1)

        ttk.Separator(self.root).pack(fill=tk.X, padx=8)
        ttk.Label(self.root, textvariable=self.status, anchor=tk.W,
                  padding=(8, 5)).pack(fill=tk.X)

    def _key_value_tree(self, parent: ttk.Frame) -> ttk.Treeview:
        tree = ttk.Treeview(parent, columns=("field", "value"),
                            show="headings", height=8)
        tree.heading("field", text="Field")
        tree.heading("value", text="Value")
        tree.column("field", width=210, stretch=False)
        tree.column("value", width=460, stretch=True)
        return tree

    def _add_table(self, title: str, columns: tuple[str, ...],
                   widths: tuple[int, ...]) -> ttk.Treeview:
        frame = ttk.Frame(self.notebook)
        tree = ttk.Treeview(frame, columns=columns, show="headings")
        vertical = ttk.Scrollbar(frame, orient=tk.VERTICAL,
                                 command=tree.yview)
        horizontal = ttk.Scrollbar(frame, orient=tk.HORIZONTAL,
                                   command=tree.xview)
        tree.configure(yscrollcommand=vertical.set,
                       xscrollcommand=horizontal.set)
        tree.grid(row=0, column=0, sticky=tk.NSEW)
        vertical.grid(row=0, column=1, sticky=tk.NS)
        horizontal.grid(row=1, column=0, sticky=tk.EW)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)
        for column, width in zip(columns, widths):
            tree.heading(column, text=column.replace("_", " ").title())
            tree.column(column, width=width, minwidth=60)
        tree.bind("<<TreeviewSelect>>",
                  lambda _event, selected=tree: self._show_selection(selected))
        self.notebook.add(frame, text=title)
        return tree

    def _choose_capture(self) -> None:
        name = filedialog.askopenfilename(
            title="Open VitaProfiler capture",
            filetypes=(("VitaProfiler captures", "*.vptrace"),
                       ("All files", "*.*")))
        if not name:
            return
        path = Path(name)
        self._start_task(
            lambda: self.controller.open_capture(path),
            self._load_capture, f"Opening {path.name}...", cancellable=False)

    def _choose_receive_output(self) -> None:
        name = filedialog.asksaveasfilename(
            title="Save received VitaProfiler capture",
            defaultextension=".vptrace",
            filetypes=(("VitaProfiler captures", "*.vptrace"),))
        if not name:
            return
        output = Path(name)
        force = output.exists()
        if force and not messagebox.askyesno(
                "Replace capture?", f"{output} already exists. Replace it?"):
            return
        try:
            port = int(self.port.get())
            if not 1 <= port <= 65535:
                raise ValueError("port must be between 1 and 65535")
        except ValueError as error:
            messagebox.showerror("Invalid receiver settings", str(error))
            return
        config = desktop.ReceiveConfig(
            output=output,
            bind=self.bind_address.get().strip() or "0.0.0.0",
            port=port,
            source=self.source_address.get().strip() or None,
            force=force,
        )

        def receive() -> desktop.LoadedCapture:
            def publish_live(loaded: desktop.LoadedCapture) -> None:
                update = (
                    loaded,
                    loaded.view_model.filter_tables(
                        "", None, DISPLAY_ROW_LIMIT),
                )
                _put_latest(self.live_updates, update)

            return self.controller.receive_capture(
                config, self.cancel_event.is_set,
                lambda address: _put_latest(
                    self.status_messages,
                    f"Listening on {address[0]}:{address[1]}..."),
                lambda count: _put_latest(
                    self.status_messages,
                    f"Receiving capture: {count:,} bytes..."),
                publish_live)

        self._start_task(receive, self._load_capture,
                         "Starting TCP receiver...", cancellable=True)

    def _choose_export(self, perfetto: bool) -> None:
        if self.loaded is None:
            return
        kind = "Perfetto" if perfetto else "decoded"
        name = filedialog.asksaveasfilename(
            title=f"Export {kind} JSON",
            defaultextension=".perfetto.json" if perfetto else ".json",
            filetypes=(("JSON", "*.json"),))
        if not name:
            return
        output = Path(name)
        force = output.exists()
        if force and not messagebox.askyesno(
                "Replace export?", f"{output} already exists. Replace it?"):
            return

        def export() -> Path:
            assert self.loaded is not None
            if perfetto:
                self.controller.export_perfetto(self.loaded, output, force)
            else:
                self.controller.export_decoded(self.loaded, output, force)
            return output

        self._start_task(
            export,
            lambda path: self.status.set(f"Wrote {kind} JSON to {path}"),
            f"Exporting {kind} JSON...", cancellable=False)

    def _start_task(self, work: Callable[[], object],
                    on_success: Callable[[object], None],
                    description: str, cancellable: bool) -> None:
        if self.busy:
            messagebox.showwarning(
                "Operation in progress",
                "Cancel or wait for the current operation to finish.")
            return
        self.busy = True
        self.cancellable = cancellable
        self.cancel_event.clear()
        self.status.set(description)
        self._set_action_state()
        future = self.executor.submit(work)
        self.root.after(50, self._poll_task, future, on_success)

    def _poll_task(self, future: concurrent.futures.Future[object],
                   on_success: Callable[[object], None]) -> None:
        self._drain_status()
        if future.done():
            _discard_pending(self.live_updates)
        else:
            self._drain_live_updates()
            self.root.after(50, self._poll_task, future, on_success)
            return
        self.busy = False
        self.cancellable = False
        self._set_action_state()
        try:
            result = future.result()
        except trace.TraceReceiveCancelled:
            self.status.set("Receiver cancelled.")
        except Exception as error:
            self.status.set(f"Error: {error}")
            messagebox.showerror("VitaProfiler error", str(error))
        else:
            on_success(result)

    def _drain_status(self) -> None:
        latest = None
        while True:
            try:
                latest = self.status_messages.get_nowait()
            except queue.Empty:
                break
        if latest is not None:
            self.status.set(latest)

    def _drain_live_updates(self) -> None:
        latest = None
        while True:
            try:
                latest = self.live_updates.get_nowait()
            except queue.Empty:
                break
        if latest is None:
            return
        loaded, rows = latest
        self._apply_loaded_capture(loaded, reset_filters=False)
        self._apply_table_rows(rows)
        self.status.set(
            f"LIVE / INCOMPLETE: {len(loaded.capture.events):,} events")

    def _set_action_state(self) -> None:
        normal = tk.DISABLED if self.busy else tk.NORMAL
        self.open_button.configure(state=normal)
        self.receive_button.configure(state=normal)
        self.cancel_button.configure(
            state=(tk.NORMAL if self.busy and self.cancellable else
                   tk.DISABLED))
        export = (
            tk.NORMAL
            if (not self.busy and self.loaded is not None and
                self.loaded.capture.complete)
            else tk.DISABLED)
        self.json_button.configure(state=export)
        self.perfetto_button.configure(state=export)

    def _cancel(self) -> None:
        if not self.cancellable:
            return
        self.cancel_event.set()
        self.status.set("Cancelling...")

    def _load_capture(self, loaded: object) -> None:
        if not isinstance(loaded, desktop.LoadedCapture):
            raise TypeError("background operation returned an invalid capture")
        self._apply_loaded_capture(loaded, reset_filters=True)
        self.status.set(
            f"Loaded {loaded.source}: "
            f"{len(loaded.capture.events):,} events. Preparing display...")
        self._refresh_tables()
        self._set_action_state()

    def _apply_loaded_capture(self, loaded: desktop.LoadedCapture,
                              reset_filters: bool) -> None:
        self.loaded = loaded
        self.model = loaded.view_model
        self._fill_key_values(self.metadata_tree, self.model.metadata_rows())
        self._fill_key_values(self.loss_tree, self.model.loss_rows())
        self.thread_combo.configure(values=(
            "All threads",
            *(f"0x{thread:08x}" for thread in self.model.threads),
        ))
        if reset_filters:
            self.thread_filter.set("All threads")
            self.filter_text.set("")

    def _clear_filter(self) -> None:
        self.filter_text.set("")
        self.thread_filter.set("All threads")
        self._refresh_tables()

    def _selected_thread(self) -> int | None:
        value = self.thread_filter.get()
        return None if value == "All threads" else int(value, 16)

    def _refresh_tables(self) -> None:
        if self.model is None:
            return
        query = self.filter_text.get()
        thread = self._selected_thread()
        model = self.model
        self._start_task(
            lambda: model.filter_tables(query, thread, DISPLAY_ROW_LIMIT),
            self._apply_table_rows, "Filtering bounded display rows...",
            cancellable=False)

    def _apply_table_rows(self, result: object) -> None:
        if not isinstance(result, desktop.TableRows):
            raise TypeError("background filter returned invalid rows")
        self.row_objects.clear()
        self._fill_rows(
            "timeline", self.timeline_tree,
            result.timeline.rows,
            lambda row: (
                row.kind, f"{row.begin_us:,}", f"{row.duration_us:,}",
                row.name, f"0x{row.thread_id:08x}",
                "-" if row.thread_generation is None else
                row.thread_generation,
                ", ".join(row.flags) or "-"))
        self._fill_rows(
            "frames", self.frame_tree,
            result.frames.rows,
            lambda row: (
                row.event_index, f"{row.timestamp_us:,}",
                "-" if row.duration_us is None else f"{row.duration_us:,}",
                "-" if row.fps is None else f"{row.fps:.2f}",
                row.name, f"0x{row.thread_id:08x}",
                "-" if row.thread_generation is None else
                row.thread_generation, row.sequence,
                ", ".join(row.flags) or "-"))
        self._fill_rows(
            "zones", self.zone_tree,
            result.zones.rows,
            lambda row: (
                row.name, f"0x{row.thread_id:08x}", f"{row.begin_us:,}",
                f"{row.duration_us:,}", row.correlation_id,
                _flag_text(row.flags)))
        self._fill_rows(
            "counters", self.counter_tree,
            result.counters.rows,
            lambda row: (
                row.event_index, f"{row.timestamp_us:,}", row.name,
                row.value, row.sample_type, f"0x{row.thread_id:08x}",
                "-" if row.thread_generation is None else
                row.thread_generation,
                ", ".join(row.flags) or "-"))
        self._fill_rows(
            "events", self.event_tree,
            result.events.rows,
            lambda row: (
                row.index, f"{row.timestamp_us:,}", row.type_name,
                self.model.capture.resolve_name(row.name_id), row.value,
                f"0x{row.thread_id:08x}", row.correlation_id,
                ", ".join(row.flag_names) or "-"))
        truncated = any((
            result.timeline.truncated, result.frames.truncated,
            result.zones.truncated,
            result.counters.truncated, result.events.truncated,
        ))
        suffix = (" (additional matching rows hidden by the display limit)"
                  if truncated else "")
        self.status.set(f"Filter applied{suffix}.")

    def _fill_rows(self, key: str, tree: ttk.Treeview, rows: tuple,
                   values: Callable[[object], tuple]) -> None:
        tree.delete(*tree.get_children())
        for index, row in enumerate(rows):
            item = f"{key}-{index}"
            tree.insert("", tk.END, iid=item, values=values(row))
            self.row_objects[(str(tree), item)] = row

    def _show_selection(self, tree: ttk.Treeview) -> None:
        selection = tree.selection()
        if not selection or self.model is None:
            return
        row = self.row_objects.get((str(tree), selection[0]))
        if row is not None:
            self._fill_key_values(self.details_tree, self.model.details(row))

    @staticmethod
    def _fill_key_values(tree: ttk.Treeview,
                         rows: tuple[tuple[str, str], ...]) -> None:
        tree.delete(*tree.get_children())
        for field, value in rows:
            tree.insert("", tk.END, values=(field, value))

    def _close(self) -> None:
        self.cancel_event.set()
        self.executor.shutdown(wait=False, cancel_futures=True)
        self.root.destroy()


def _flag_text(flags: int) -> str:
    names = [name for bit, name in trace.EVENT_FLAG_NAMES.items()
             if flags & bit]
    return ", ".join(names) or "-"


def main() -> int:
    root = tk.Tk()
    ProfilerApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
