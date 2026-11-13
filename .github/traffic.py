"""Keeps GitHub's counts of the repository, which it holds for 14 days only, as CSV files.

days.csv: views, visitors, clones and cloners a day; a day fetched again takes the newer
counts, as the newest day is counted only in part. referrers.csv: where visitors came from
over the 14 days before each run. totals.csv: stars, forks, watchers and release downloads
at each run. Run by .github/workflows/traffic.yml with the folder to write in.
"""
import csv
import datetime
import json
import os
import sys
import urllib.request
from pathlib import Path

REPOSITORY = os.environ["GITHUB_REPOSITORY"]
TOKEN = os.environ["TRAFFIC_TOKEN"]


def get(path):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{REPOSITORY}{path}",
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Accept": "application/vnd.github+json",
                 "User-Agent": "traffic"})
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def merge(file, fields, rows, key):
    """Writes the file's rows and these, a row of the same key replaced by the new one."""
    kept = {}
    if file.exists():
        with file.open(newline="") as text:
            kept = {key(row): row for row in csv.DictReader(text)}
    kept.update({key(row): row for row in rows})
    with file.open("w", newline="") as text:
        writer = csv.DictWriter(text, fields)
        writer.writeheader()
        writer.writerows(sorted(kept.values(), key=key))


def main(folder):
    today = datetime.date.today().isoformat()
    views = {day["timestamp"][:10]: day for day in get("/traffic/views")["views"]}
    clones = {day["timestamp"][:10]: day for day in get("/traffic/clones")["clones"]}
    days = [{"date": date,
             "views": views.get(date, {}).get("count", 0),
             "visitors": views.get(date, {}).get("uniques", 0),
             "clones": clones.get(date, {}).get("count", 0),
             "cloners": clones.get(date, {}).get("uniques", 0)}
            for date in sorted(views.keys() | clones.keys())]
    merge(folder / "days.csv", ["date", "views", "visitors", "clones", "cloners"], days,
          lambda row: row["date"])

    referrers = [{"date": today, "referrer": each["referrer"], "views": each["count"],
                  "visitors": each["uniques"]}
                 for each in get("/traffic/popular/referrers")]
    merge(folder / "referrers.csv", ["date", "referrer", "views", "visitors"], referrers,
          lambda row: (row["date"], row["referrer"]))

    repository = get("")
    downloads = sum(asset["download_count"]
                    for release in get("/releases") for asset in release["assets"])
    totals = [{"date": today, "stars": repository["stargazers_count"],
               "forks": repository["forks_count"],
               "watchers": repository["subscribers_count"], "downloads": downloads}]
    merge(folder / "totals.csv", ["date", "stars", "forks", "watchers", "downloads"], totals,
          lambda row: row["date"])


if __name__ == "__main__":
    main(Path(sys.argv[1]))
