import csv
import sys
from pathlib import Path
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: python scripts/plots.py data/results_student104.csv")
    raise SystemExit(2)

path = Path(sys.argv[1])
with path.open(newline="", encoding="utf-8-sig") as f:
    rows = list(csv.DictReader(f))
if not rows:
    raise SystemExit("CSV is empty")

groups = {}
for row in rows:
    key = (row["lock_mode"], row["distribution"])
    groups.setdefault(key, []).append(row)

labels = [f"{k[0]}\\n{k[1]}" for k in groups]
means_time = [sum(float(r["elapsed_ms"]) for r in v) / len(v) for v in groups.values()]
means_wait = [sum(float(r["max_wait_us"]) for r in v) / len(v) for v in groups.values()]
out1 = path.with_name(path.stem + "_execution_time.png")
out2 = path.with_name(path.stem + "_max_wait.png")

plt.figure(figsize=(9, 5))
plt.bar(labels, means_time)
plt.ylabel("Среднее время выполнения, мс")
plt.title("Сравнение времени выполнения (среднее по повторам)")
plt.tight_layout()
plt.savefig(out1, dpi=160)
plt.close()

plt.figure(figsize=(9, 5))
plt.bar(labels, means_wait)
plt.ylabel("Среднее максимальное ожидание, мкс")
plt.title("Сравнение максимального ожидания блокировки")
plt.tight_layout()
plt.savefig(out2, dpi=160)
plt.close()
print(f"Saved: {out1}")
print(f"Saved: {out2}")
