#!/usr/bin/env python3

import argparse
import csv
import json
import os
import sys
import threading
import time
from datetime import datetime, timezone

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Потрібен pyserial:  pip install pyserial")

# Поля, які друкує Base (порядок фіксований прошивкою)
BASE_FIELDS = [
    "run_id", "node_id", "role", "seq_num", "config_id", "freq_hz", "sf", "bw_hz", "cr",
    "tx_power_dbm", "payload_len", "tx_interval_ms", "toa_us", "rssi_dbm", "snr_db", "rx_ok",
    "received", "lost", "pdr",
]

# Рекомендований header з гайда (розділ 8) + додаткові поля logger'а і лічильники Base
CSV_HEADER = [
    "timestamp_utc", "team_id", "run_id", "node_id", "role", "seq_num", "location_tag",
    "distance_m_est", "config_id", "freq_hz", "sf", "bw_hz", "cr", "tx_power_dbm",
    "payload_len", "tx_interval_ms", "toa_us", "rssi_dbm", "snr_db", "rx_ok",
    "node_height_m", "env_notes", "received", "lost", "pdr",
]

INT_FIELDS = {"run_id", "node_id", "seq_num", "config_id", "freq_hz", "sf", "bw_hz", "cr",
              "tx_power_dbm", "payload_len", "tx_interval_ms", "toa_us", "rx_ok", "received", "lost"}
FLOAT_FIELDS = {"rssi_dbm", "snr_db", "pdr"}


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def parse_data_line(line):
    """Повертає dict або кидає ValueError, якщо рядок не схожий на дані Base."""
    parts = line.split(",")
    if len(parts) != len(BASE_FIELDS):
        raise ValueError(f"очікувалось {len(BASE_FIELDS)} полів, отримано {len(parts)}")
    rec = dict(zip(BASE_FIELDS, (p.strip() for p in parts)))
    for k in INT_FIELDS:
        int(rec[k])
    for k in FLOAT_FIELDS:
        float(rec[k])
    return rec


class Logger:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.ctx = {"location_tag": args.location, "distance_m_est": args.distance,
                    "node_height_m": args.height, "env_notes": args.notes}
        self.stop = threading.Event()
        self.ser = None
        self.rows = 0
        self.rejected = 0
        self.runs = {}
        self.meta_dirty = False
        self.started = utc_now()
        self.csv_file = None
        self.writer = None

    # ---------- файли ----------
    def open_csv(self):
        path = self.args.out
        exists = os.path.exists(path) and os.path.getsize(path) > 0
        if exists:
            with open(path, newline="", encoding="utf-8") as f:
                first = next(csv.reader(f), [])
            if first != CSV_HEADER:
                sys.exit(f"{path} має інший header. Вкажіть інший --out або перейменуйте старий файл.")
        self.csv_file = open(path, "a", newline="", encoding="utf-8")
        self.writer = csv.writer(self.csv_file)
        if not exists:
            self.writer.writerow(CSV_HEADER)
            self.csv_file.flush()

    def write_meta(self):
        meta = {
            "logger": {"team_id": self.args.team, "raw_csv": self.args.out, "port": self.args.port,
                       "baud": self.args.baud, "started_utc": self.started, "updated_utc": utc_now(),
                       "rows_written": self.rows, "rejected_lines": self.rejected},
            "runs": self.runs,
        }
        tmp = self.args.meta + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        os.replace(tmp, self.args.meta)
        self.meta_dirty = False

    def update_run_meta(self, rec, ts, ctx):
        rid = rec["run_id"]
        r = self.runs.setdefault(rid, {
            "run_id": int(rid), "first_utc": ts, "config_ids": [], "node_ids": [],
            "rows": 0, "locations": [], "payload_len": [], "tx_interval_ms": [],
        })
        r["last_utc"] = ts
        r["rows"] += 1
        for key, val in (("config_ids", int(rec["config_id"])), ("node_ids", int(rec["node_id"]))):
            if val not in r[key]:
                r[key].append(val)
        for key in ("payload_len", "tx_interval_ms"):
            if int(rec[key]) not in r[key]:
                r[key].append(int(rec[key]))
        loc = {"location_tag": ctx["location_tag"], "distance_m_est": ctx["distance_m_est"],
               "node_height_m": ctx["node_height_m"], "env_notes": ctx["env_notes"]}
        if not r["locations"] or r["locations"][-1] != loc:
            r["locations"].append(loc)
        r["last_seq_num"] = int(rec["seq_num"])
        r["received"] = int(rec["received"])   # останні значення лічильників Base (per node)
        r["lost"] = int(rec["lost"])
        r["pdr"] = float(rec["pdr"])
        self.meta_dirty = True

    # ---------- обробка рядка ----------
    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        if line.startswith("#"):
            if self.args.verbose:
                print(f"  {line}")
            return
        ts = utc_now()
        try:
            rec = parse_data_line(line)
        except ValueError as e:
            # Нічого не викидаємо мовчки: пишемо в окремий файл
            self.rejected += 1
            with open(self.args.rejected, "a", encoding="utf-8") as f:
                f.write(f"{ts}\t{e}\t{line}\n")
            print(f"[!] відхилений рядок ({e}): {line}")
            return
        with self.lock:
            ctx = dict(self.ctx)
        row = {**rec, **ctx, "timestamp_utc": ts, "team_id": self.args.team}
        self.writer.writerow([row[c] for c in CSV_HEADER])
        self.csv_file.flush()
        self.rows += 1
        self.update_run_meta(rec, ts, ctx)
        print(f"[{self.rows:5d}] run={rec['run_id']} node={rec['node_id']} seq={rec['seq_num']} "
              f"cfg={rec['config_id']} RSSI={rec['rssi_dbm']} SNR={rec['snr_db']} "
              f"rx={rec['received']} lost={rec['lost']} PDR={rec['pdr']}% @{ctx['location_tag'] or '-'}")

    # ---------- порт ----------
    def find_port(self):
        ports = list(serial.tools.list_ports.comports())
        for p in ports:
            desc = f"{p.description} {p.manufacturer} {p.hwid}".lower()
            if "cp210" in desc or "silicon labs" in desc or "10c4:ea60" in desc:
                return p.device
        if len(ports) == 1:
            return ports[0].device
        return None

    def open_serial(self):
        port = self.args.port or self.find_port()
        if not port:
            avail = ", ".join(p.device for p in serial.tools.list_ports.comports()) or "немає"
            raise serial.SerialException(f"порт Base не знайдено (доступні: {avail}); вкажіть --port")
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, self.args.baud, 1
        s.dtr = False   # щоб відкриття порту не перезавантажувало ESP32
        s.rts = False
        s.open()
        self.args.port = port
        return s

    def serial_loop(self):
        while not self.stop.is_set():
            try:
                self.ser = self.open_serial()
                print(f"[+] підключено до {self.args.port} @ {self.args.baud}")
                buf = b""
                last_meta = time.time()
                while not self.stop.is_set():
                    chunk = self.ser.read(256)
                    if chunk:
                        buf += chunk
                        while b"\n" in buf:
                            raw, buf = buf.split(b"\n", 1)
                            self.handle_line(raw.decode("utf-8", errors="replace"))
                    if self.meta_dirty and time.time() - last_meta > 5:
                        self.write_meta()
                        last_meta = time.time()
            except (serial.SerialException, OSError) as e:
                print(f"[-] {e}; повтор через 2 с...")
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.stop.wait(2)

    # ---------- команди користувача ----------
    def send_to_base(self, cmd):
        if self.ser and self.ser.is_open:
            self.ser.write((cmd + "\n").encode())
            print(f"[>] Base: {cmd}")
        else:
            print("[-] Base не підключений")

    def command_loop(self):
        print("Команди: loc <TAG> [dist_m] [height_m] [notes] | note <text> | !<cmd для Base> | status | help | quit")
        while not self.stop.is_set():
            try:
                line = input().strip()
            except (EOFError, KeyboardInterrupt):
                return
            if not line:
                continue
            if line == "quit":
                self.stop.set()
            elif line == "help":
                print(__doc__)
            elif line == "status":
                with self.lock:
                    print(f"rows={self.rows} rejected={self.rejected} ctx={self.ctx}")
            elif line.startswith("!"):
                self.send_to_base(line[1:].strip())
            elif line.startswith("note "):
                with self.lock:
                    self.ctx["env_notes"] = line[5:].strip()
                print(f"[=] env_notes = {self.ctx['env_notes']}")
            elif line.startswith("loc "):
                parts = line.split(None, 4)
                with self.lock:
                    self.ctx["location_tag"] = parts[1]
                    if len(parts) > 2:
                        self.ctx["distance_m_est"] = parts[2]
                    if len(parts) > 3:
                        self.ctx["node_height_m"] = parts[3]
                    if len(parts) > 4:
                        self.ctx["env_notes"] = parts[4]
                print(f"[=] {self.ctx}")
            else:
                print("невідома команда (help)")

    def run(self):
        self.open_csv()
        print(f"[i] пишу в {self.args.out}, metadata: {self.args.meta}, team={self.args.team}")
        threading.Thread(target=self.command_loop, daemon=True).start()
        try:
            self.serial_loop()
        except KeyboardInterrupt:
            self.stop.set()
        finally:
            self.stop.set()
            self.write_meta()
            if self.csv_file:
                self.csv_file.close()
            print(f"\n[i] завершено: {self.rows} рядків, відхилено {self.rejected}")


def main():
    ap = argparse.ArgumentParser(description="LoRa PHY Quest laptop logger")
    ap.add_argument("--port", help="COM-порт Base (напр. COM5 або /dev/ttyUSB0); за замовчуванням автопошук")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--team", default="team1", help="team_id")
    ap.add_argument("--out", default="raw.csv")
    ap.add_argument("--meta", default="run_metadata.json")
    ap.add_argument("--rejected", default="rejected_lines.log")
    ap.add_argument("--location", default="", help="початковий location_tag")
    ap.add_argument("--distance", default="", help="початкова distance_m_est")
    ap.add_argument("--height", default="", help="початкова node_height_m")
    ap.add_argument("--notes", default="", help="початкові environment notes")
    ap.add_argument("-v", "--verbose", action="store_true", help="показувати службові '#' рядки")
    Logger(ap.parse_args()).run()


if __name__ == "__main__":
    main()
