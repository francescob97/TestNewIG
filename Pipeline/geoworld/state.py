"""
Stato della pipeline: e' cio' che la rende idempotente e riavviabile.

L'idea e' minimale e non richiede un database. Ogni stadio ha un'IMPRONTA
calcolata sui suoi input e sui suoi parametri. A stadio concluso, impronta e
lista degli output finiscono in _work/state.json. Al rilancio, uno stadio viene
saltato se e solo se:

  - l'impronta coincide (stessi input, stessi parametri), E
  - tutti i suoi output esistono ancora con la dimensione registrata.

La seconda condizione non e' pedanteria: cancellare un intermedio per fare
spazio e' una cosa che si fa, e la pipeline deve accorgersene invece di
proseguire su un file che non c'e' piu'.

Nell'impronta degli input entrano percorso, dimensione e data di modifica, non
il contenuto: fare l'hash di decine di GB a ogni lancio costerebbe piu' del
lavoro che si vuole evitare.
"""

from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime, timezone


class PipelineState:
    def __init__(self, path: str):
        self.path = path
        self.data: dict = {}
        if os.path.exists(path):
            try:
                with open(path, encoding="utf-8") as handle:
                    self.data = json.load(handle)
            except (json.JSONDecodeError, OSError):
                # Uno stato illeggibile non deve bloccare: al massimo si rifa' tutto.
                self.data = {}

    # --- impronte ---------------------------------------------------------

    @staticmethod
    def fingerprint(parameters: dict, input_paths: list[str] | None = None) -> str:
        digest = hashlib.sha256()
        digest.update(json.dumps(parameters, sort_keys=True, default=str).encode("utf-8"))
        for path in sorted(input_paths or []):
            try:
                stat = os.stat(path)
                digest.update(f"{path}|{stat.st_size}|{int(stat.st_mtime)}".encode("utf-8"))
            except OSError:
                digest.update(f"{path}|assente".encode("utf-8"))
        return digest.hexdigest()

    # --- interrogazione e aggiornamento -----------------------------------

    def is_complete(self, stage: str, fingerprint: str) -> bool:
        record = self.data.get(stage)
        if not record or record.get("fingerprint") != fingerprint:
            return False

        for output in record.get("outputs", []):
            path = output["path"]
            if not os.path.exists(path):
                return False
            if output.get("size") is not None and os.path.getsize(path) != output["size"]:
                return False
        return True

    def mark_complete(self, stage: str, fingerprint: str, outputs: list[str],
                      info: dict | None = None) -> None:
        self.data[stage] = {
            "fingerprint": fingerprint,
            "completedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "outputs": [{"path": path,
                         "size": os.path.getsize(path) if os.path.exists(path) else None}
                        for path in outputs],
            "info": info or {},
        }
        self.save()

    def info(self, stage: str) -> dict:
        return self.data.get(stage, {}).get("info", {})

    def invalidate(self, stage: str) -> None:
        self.data.pop(stage, None)

    def save(self) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        temporary = f"{self.path}.tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(self.data, handle, indent="\t")
        os.replace(temporary, self.path)
