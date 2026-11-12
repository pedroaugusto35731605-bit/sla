#!/usr/bin/env python3
"""
SeriesS-Emu — interface gráfica (biblioteca de jogos).

Estilo "serious/sleek": escuro, tipográfico, com um único tom de destaque.
Funções:
  • Biblioteca em lista com capa, tags de hardware ("4TFLOPS", "Zen 2", "1440p"…)
  • Baixar ISO: campo de URL + "Download & Install"; a barra de progresso simula a
    gravação no SSD NVMe do Series S (~2,4 GB/s). O download é SIMULADO: nenhum
    byte sai da URL. Ao terminar, o jogo entra na lista sem reiniciar o app.
  • Importar ISO local (metadados, tags e capa lidos pelo núcleo C++: xss-emu info)
  • Iniciar (boot do núcleo com console) e Benchmark de I/O (xss-emu bench)

Biblioteca: $XSS_HOME/library.json  (padrão: ~/.seriess-emu/library.json)

    pip install -r requirements.txt
    python ui/main_window.py
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import shutil
import sys
import threading
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote, urlparse

from PyQt6 import uic
from PyQt6.QtCore import (QEasingCurve, QEvent, QObject, QProcess, QProcessEnvironment, QPropertyAnimation, QRectF,
                          QSize, Qt, QTimer, pyqtSignal)
from PyQt6.QtGui import (QAction, QColor, QFont, QFontMetrics, QKeySequence, QLinearGradient, QPainter,
                         QPainterPath, QPen, QPixmap, QTextCursor)
from PyQt6.QtWidgets import (QApplication, QDialog, QFileDialog, QFrame, QHBoxLayout, QLabel, QListWidget,
                             QListWidgetItem, QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar,
                             QPushButton, QSizePolicy, QStyle, QStyledItemDelegate, QVBoxLayout, QWidget)

APP_DIR = Path(__file__).resolve().parent
PROJECT_DIR = APP_DIR.parent
UI_FILE = APP_DIR / "game_library.ui"

# ---------------------------------------------------------------------------
#  Hardware (espelha core/hardware_spec.hpp)
# ---------------------------------------------------------------------------
SSD_CAPACITY = 512 * 10**9            # 512 GB
SSD_RAW_BPS = 2.4e9                   # 2,4 GB/s
USER_SPACE_GB, SYSTEM_GB = 7, 3
HW_TAGS = ["4TFLOPS", "Zen 2", "1440p"]
ACCEPTED_EXTENSIONS = (".iso", ".xvc", ".xiso", ".xvd")

# ---------------------------------------------------------------------------
#  Tema
# ---------------------------------------------------------------------------
BG, SURFACE, SURFACE_2, BORDER = "#0B0C0E", "#121316", "#191B1F", "#23262B"
TEXT, MUTED, FAINT = "#E9EAEC", "#8A8F96", "#5A5F66"
ACCENT = "#4FD18B"                     # verde contido, usado com parcimônia

STYLESHEET = f"""
* {{ font-family: "Inter", "Segoe UI", "SF Pro Text", "Noto Sans", sans-serif; color: {TEXT}; font-size: 13px; }}
QMainWindow, #centralwidget, #content {{ background: {BG}; }}
#rail {{ background: {SURFACE}; border-right: 1px solid {BORDER}; }}
#brand {{ font-size: 20px; font-weight: 800; letter-spacing: 4px; }}
#brandSub {{ color: {FAINT}; font-size: 11px; letter-spacing: 3px; }}
#sectionHw, #sectionMem, #sectionSsd, #sectionSys, #installTitle {{
    color: {FAINT}; font-size: 10px; font-weight: 700; letter-spacing: 2px;
}}
#btnFirmware {{ padding: 7px 10px; font-size: 12px; margin-top: 4px; }}
#hwInfo, #memInfo, #ssdInfo, #fwInfo {{ color: {MUTED}; font-size: 12px; line-height: 150%; }}
#coreStatus {{ color: {FAINT}; font-size: 11px; }}
#pageTitle {{ font-size: 26px; font-weight: 700; }}
#gameCount {{ color: {FAINT}; font-size: 15px; padding: 8px 0 0 6px; }}
QLineEdit {{
    background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 8px; padding: 9px 12px;
    selection-background-color: {ACCENT}; selection-color: {BG};
}}
QLineEdit:focus {{ border-color: {MUTED}; }}
QPushButton {{
    background: {SURFACE_2}; border: 1px solid {BORDER}; border-radius: 8px; padding: 9px 16px; font-weight: 600;
}}
QPushButton:hover {{ border-color: {FAINT}; }}
QPushButton:disabled {{ color: {FAINT}; background: {SURFACE}; }}
#btnInstall, #btnLaunch {{ background: {TEXT}; color: {BG}; border: 1px solid {TEXT}; }}
#btnInstall:hover, #btnLaunch:hover {{ background: white; }}
#btnInstall:disabled, #btnLaunch:disabled {{ background: {SURFACE_2}; color: {FAINT}; border-color: {BORDER}; }}
#btnLaunch {{ padding: 12px; font-size: 14px; }}
#btnRemove {{ color: #D27373; }}
#installCard, #detailPanel {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 12px; }}
#specsFrame {{ background: transparent; border-top: 1px solid {BORDER}; padding-top: 10px; }}
#kRes, #kFps, #kTid, #kSize, #kFmt, #kSrc {{ color: {FAINT}; font-size: 12px; }}
#detailTitle {{ font-size: 20px; font-weight: 700; }}
#detailPublisher {{ color: {MUTED}; }}
QListWidget#gameList {{ background: transparent; border: none; outline: none; }}
#emptyState {{ color: {FAINT}; font-size: 14px; }}
QProgressBar {{ background: {SURFACE_2}; border: none; border-radius: 2px; max-height: 4px; min-height: 4px; }}
QProgressBar::chunk {{ background: {ACCENT}; border-radius: 2px; }}
QSplitter::handle {{ background: transparent; width: 16px; }}
QStatusBar {{ background: {BG}; color: {FAINT}; border-top: 1px solid {BORDER}; }}
QScrollBar:vertical {{ background: transparent; width: 8px; }}
QScrollBar::handle:vertical {{ background: {BORDER}; border-radius: 4px; min-height: 30px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QDialog, QMessageBox {{ background: {SURFACE}; }}
QPlainTextEdit {{ background: {BG}; border: 1px solid {BORDER}; border-radius: 8px;
    font-family: "JetBrains Mono", "Cascadia Mono", "DejaVu Sans Mono", monospace; font-size: 11px; color: #C9CDD2; }}
"""


# ---------------------------------------------------------------------------
#  Modelo
# ---------------------------------------------------------------------------
def library_root() -> Path:
    if os.environ.get("XSS_HOME"):
        return Path(os.environ["XSS_HOME"])
    if sys.platform == "win32" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "SeriesS-Emu"
    return Path.home() / ".seriess-emu"


@dataclass
class Game:
    id: str
    title: str
    title_id: str = ""
    publisher: str = ""
    path: str = ""
    format: str = ""
    thumbnail: str = ""
    source: str = "import"
    added_at: str = ""
    size_bytes: int = 0
    target_width: int = 2560
    target_height: int = 1440
    target_fps: int = 60
    tags: list[str] = field(default_factory=list)
    extra: dict = field(default_factory=dict)

    KNOWN = ("id", "title", "title_id", "publisher", "path", "format", "thumbnail", "source", "added_at",
             "size_bytes", "target_width", "target_height", "target_fps", "tags")

    @classmethod
    def from_dict(cls, d: dict) -> "Game":
        return cls(**{k: d[k] for k in cls.KNOWN if k in d}, extra={k: v for k, v in d.items() if k not in cls.KNOWN})

    def to_dict(self) -> dict:
        d = {k: getattr(self, k) for k in self.KNOWN}
        d.update(self.extra)
        return d


class Library:
    def __init__(self, root: Path | None = None):
        self.root = root or library_root()
        self.file = self.root / "library.json"
        self.games: list[Game] = []
        (self.root / "thumbnails").mkdir(parents=True, exist_ok=True)

    def load(self) -> None:
        try:
            data = json.loads(self.file.read_text(encoding="utf-8"))
            self.games = [Game.from_dict(g) for g in data.get("games", [])]
        except FileNotFoundError:
            self.games = []
        except (json.JSONDecodeError, TypeError) as e:
            backup = self.file.with_suffix(".corrupt.json")
            shutil.copy(self.file, backup)
            print(f"[XSS-UI] library.json inválido ({e}); cópia em {backup}", file=sys.stderr)
            self.games = []

    def save(self) -> None:
        tmp = self.file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps({"version": 1, "games": [g.to_dict() for g in self.games]},
                                  indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self.file)

    def upsert(self, game: Game) -> None:
        for i, g in enumerate(self.games):
            if g.id == game.id:
                self.games[i] = game
                return
        self.games.append(game)

    def remove(self, gid: str) -> None:
        self.games = [g for g in self.games if g.id != gid]

    def used_bytes(self) -> int:
        return sum(g.size_bytes for g in self.games)


def find_core() -> str | None:
    exe = "xss-emu.exe" if sys.platform == "win32" else "xss-emu"
    cands = [os.environ.get("XSS_CORE", "")]
    for sub in ("", "Release", "Debug", "portable", "zen2", "portable/Release", "zen2/Release"):
        cands.append(str(PROJECT_DIR / "build" / sub / exe))
    for c in cands:
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            return c
    return shutil.which(exe)


# ---------------------------------------------------------------------------
#  Utilidades visuais
# ---------------------------------------------------------------------------
def gb(n: float) -> str:
    """Tamanho em GB decimais (como consoles e lojas exibem)."""
    if n >= 10**9:
        return f"{n / 10**9:.1f} GB"
    if n >= 10**6:
        return f"{n / 10**6:.0f} MB"
    return f"{n / 1000:.0f} KB"


def generated_cover(title: str, w: int, h: int) -> QPixmap:
    seed = int(hashlib.sha1(title.encode()).hexdigest()[:6], 16)
    pm = QPixmap(w, h)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    g = QLinearGradient(0, 0, w, h)
    g.setColorAt(0, QColor.fromHsv(seed % 360, 40, 58))
    g.setColorAt(1, QColor.fromHsv((seed // 7) % 360, 50, 22))
    p.fillRect(0, 0, w, h, g)
    p.setPen(QPen(QColor(255, 255, 255, 30), 1))
    for i in range(-h, w, max(6, w // 8)):           # hachura diagonal discreta
        p.drawLine(i, h, i + h, 0)
    initials = "".join(word[0] for word in title.split()[:2]).upper() or "?"
    f = QFont()
    f.setPixelSize(max(12, int(h * 0.30)))
    f.setWeight(QFont.Weight.Black)
    p.setFont(f)
    p.setPen(QColor(255, 255, 255, 220))
    p.drawText(QRectF(0, 0, w, h), Qt.AlignmentFlag.AlignCenter, initials)
    p.end()
    return pm


_covers: dict[tuple, QPixmap] = {}


def cover_for(game: Game, w: int, h: int, radius: float = 6) -> QPixmap:
    key = (game.id, game.thumbnail, w, h, radius)
    if key in _covers:
        return _covers[key]
    src = QPixmap(game.thumbnail) if game.thumbnail and Path(game.thumbnail).is_file() else QPixmap()
    if src.isNull():
        src = generated_cover(game.title, w, h)
    else:
        src = src.scaled(w, h, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation)
        src = src.copy((src.width() - w) // 2, (src.height() - h) // 2, w, h)
    out = QPixmap(w, h)
    out.fill(Qt.GlobalColor.transparent)
    p = QPainter(out)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    path = QPainterPath()
    path.addRoundedRect(QRectF(0, 0, w, h), radius, radius)
    p.setClipPath(path)
    p.drawPixmap(0, 0, src)
    p.end()
    _covers[key] = out
    return out


def draw_chips(p: QPainter, x: float, y: float, tags: list[str], max_x: float) -> None:
    f = QFont()
    f.setPixelSize(10)
    f.setWeight(QFont.Weight.DemiBold)
    p.setFont(f)
    fm = QFontMetrics(f)
    for t in tags:
        w = fm.horizontalAdvance(t) + 14
        if x + w > max_x:
            break
        r = QRectF(x, y, w, 18)
        p.setPen(QPen(QColor(BORDER), 1))
        p.setBrush(QColor(SURFACE_2))
        p.drawRoundedRect(r, 4, 4)
        p.setPen(QColor(ACCENT if t in HW_TAGS else MUTED))
        p.drawText(r, Qt.AlignmentFlag.AlignCenter, t)
        x += w + 6


class GameRowDelegate(QStyledItemDelegate):
    ROW_H = 92

    def sizeHint(self, option, index) -> QSize:  # noqa: N802 (API Qt)
        return QSize(400, self.ROW_H)

    def paint(self, p: QPainter, option, index) -> None:
        game: Game = index.data(Qt.ItemDataRole.UserRole)
        if game is None:
            return
        p.save()
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(option.rect).adjusted(0, 4, -4, -4)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hover = bool(option.state & QStyle.StateFlag.State_MouseOver)
        if selected or hover:
            p.setPen(QPen(QColor(FAINT if selected else BORDER), 1))
            p.setBrush(QColor(SURFACE_2 if selected else SURFACE))
            p.drawRoundedRect(r, 10, 10)
        if selected:   # barra de seleção à esquerda
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(ACCENT))
            p.drawRoundedRect(QRectF(r.x(), r.y() + 18, 3, r.height() - 36), 1.5, 1.5)

        cov_h = int(r.height() - 16)
        cov_w = int(cov_h * 0.75)
        p.drawPixmap(int(r.x() + 14), int(r.y() + 8), cover_for(game, cov_w, cov_h))

        x = r.x() + 14 + cov_w + 16
        right = r.right() - 16
        f = QFont()
        f.setPixelSize(15)
        f.setWeight(QFont.Weight.DemiBold)
        p.setFont(f)
        p.setPen(QColor(TEXT))
        size_txt = gb(game.size_bytes) if game.size_bytes else ""
        f2 = QFont()
        f2.setPixelSize(12)
        fm2 = QFontMetrics(f2)
        title_w = right - x - fm2.horizontalAdvance(size_txt) - 16
        p.drawText(QRectF(x, r.y() + 12, title_w, 20), Qt.AlignmentFlag.AlignLeft,
                   QFontMetrics(f).elidedText(game.title, Qt.TextElideMode.ElideRight, int(title_w)))
        p.setFont(f2)
        p.setPen(QColor(MUTED))
        p.drawText(QRectF(right - 120, r.y() + 14, 120, 18), Qt.AlignmentFlag.AlignRight, size_txt)
        sub = " · ".join(s for s in (game.publisher, game.title_id) if s) or game.format
        p.setPen(QColor(FAINT))
        p.drawText(QRectF(x, r.y() + 34, right - x, 16), Qt.AlignmentFlag.AlignLeft, sub)
        draw_chips(p, x, r.y() + 56, game.tags, right)
        p.restore()


class MemoryBar(QWidget):
    """Barra 7 GB (título) + 3 GB (sistema), proporcional aos 10 GB."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(10)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def paintEvent(self, _e) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), 6
        split = w * USER_SPACE_GB / (USER_SPACE_GB + SYSTEM_GB)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(ACCENT))
        p.drawRoundedRect(QRectF(0, 2, split - 2, h), 3, 3)
        p.setBrush(QColor(FAINT))
        p.drawRoundedRect(QRectF(split, 2, w - split, h), 3, 3)
        p.end()


# ---------------------------------------------------------------------------
#  Instalação (download simulado + gravação no SSD a ~2,4 GB/s)
# ---------------------------------------------------------------------------
def title_from_url(url: str) -> tuple[str, str]:
    name = unquote(Path(urlparse(url).path).name) or "jogo.iso"
    stem = Path(name).stem
    for ch in "._-+":
        stem = stem.replace(ch, " ")
    title = " ".join(w if w.isupper() else w.capitalize() for w in stem.split()) or "Jogo sem título"
    return name, title


def install_size(url: str) -> int:
    return random.Random(url).randint(6, 40) * 10**9      # 6–40 GB, típico de jogos Series S


class InstallRow(QFrame):
    """Uma instalação na fila. SIMULADA: o tamanho é sintético e nada é baixado da URL."""

    finished = pyqtSignal(object)    # Game
    cancelled = pyqtSignal(object)   # self

    TICK_MS = 40

    def __init__(self, url: str, root: Path, parent=None):
        super().__init__(parent)
        self.url = url
        self.file_name, self.title = title_from_url(url)
        self.total = install_size(url)
        self.done = 0
        self.dest = root / "games" / self.file_name
        self.started = time.monotonic()
        self.last = self.started
        self._rng = random.Random(url + "speed")

        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)
        top = QHBoxLayout()
        self.name = QLabel(f"<b>{self.title}</b>&nbsp;&nbsp;<span style='color:{FAINT}'>{self.file_name}</span>")
        self.info = QLabel("Conectando…")
        self.info.setStyleSheet(f"color:{MUTED}; font-size:12px; font-family:'JetBrains Mono','DejaVu Sans Mono',monospace;")
        self.cancel_btn = QPushButton("Cancelar")
        self.cancel_btn.setStyleSheet("padding:3px 10px; font-size:11px;")
        self.cancel_btn.clicked.connect(self.cancel)
        top.addWidget(self.name, 1)
        top.addWidget(self.info)
        top.addWidget(self.cancel_btn)
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.bar.setTextVisible(False)
        lay.addLayout(top)
        lay.addWidget(self.bar)

        self._anim = QPropertyAnimation(self.bar, b"value", self)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(self.TICK_MS)

    def _tick(self) -> None:
        now = time.monotonic()
        if now - self.started < 0.6:                       # handshake
            self.last = now
            return
        dt = now - self.last
        self.last = now
        # Gravação sequencial no NVMe: ~2,4 GB/s com pequenas variações de throttling
        speed = SSD_RAW_BPS * self._rng.uniform(0.93, 1.0)
        self.done = min(self.total, self.done + int(speed * dt))
        remaining = (self.total - self.done) / speed
        self.info.setText(f"Gravando no SSD  {speed / 1e9:4.2f} GB/s   {gb(self.done)} / {gb(self.total)}   {remaining:3.0f} s")
        self._anim.stop()
        self._anim.setStartValue(self.bar.value())
        self._anim.setEndValue(int(1000 * self.done / self.total))
        self._anim.setDuration(self.TICK_MS * 2)
        self._anim.start()
        if self.done >= self.total:
            self._complete()

    def _complete(self) -> None:
        self._timer.stop()
        self._anim.stop()
        self.bar.setValue(1000)
        elapsed = time.monotonic() - self.started
        self.info.setText(f"Instalado  {gb(self.total)} em {elapsed:.1f} s")
        self.cancel_btn.hide()
        ext = Path(self.file_name).suffix.lower()
        game = Game(
            id="dl-" + hashlib.sha1(self.url.encode()).hexdigest()[:10], title=self.title, path=str(self.dest),
            format="XVD/XVC" if ext in (".xvc", ".xvd") else "XDVDFS/GDF", source="download",
            added_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), size_bytes=self.total,
            tags=HW_TAGS + ["RDNA 2", "4K upscale", "DirectStorage"],
            extra={"download_url": self.url, "mock": True},
        )
        self.finished.emit(game)
        QTimer.singleShot(3500, self.deleteLater)

    def cancel(self) -> None:
        self._timer.stop()
        self._anim.stop()
        self.info.setText("Cancelado")
        self.cancel_btn.hide()
        self.cancelled.emit(self)
        QTimer.singleShot(1200, self.deleteLater)


# ---------------------------------------------------------------------------
#  Console do núcleo
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
#  Firmware do sistema (Atualização Offline do Sistema: OSU1/OSU2/OSU3)
# ---------------------------------------------------------------------------
def firmware_root(lib_root: Path) -> Path:
    return lib_root / "firmware"          # o núcleo usa o mesmo: $XSS_HOME/firmware


def installed_firmware_version(lib_root: Path) -> str | None:
    try:
        v = (firmware_root(lib_root) / "current").read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return v or None


class ZipExtractor(QObject):
    """Extrai o OSU1.zip numa thread, emitindo progresso. Protegido contra zip-slip."""

    progress = pyqtSignal(int, int, str)     # bytes feitos, total, arquivo atual
    done = pyqtSignal(bool, str)             # sucesso, mensagem de erro

    CHUNK = 4 * 1024 * 1024

    def __init__(self, zip_path: Path, dest: Path):
        super().__init__()
        self.zip_path, self.dest = zip_path, dest
        self.cancelled = False

    def start(self) -> None:
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        try:
            dest = self.dest.resolve()
            with zipfile.ZipFile(self.zip_path) as z:
                members = [m for m in z.infolist() if not m.is_dir()]
                if not any("$systemupdate" in m.filename.lower() or m.filename.lower().endswith(".xvd")
                           for m in members):
                    self.done.emit(False, "o .zip não contém a pasta $SystemUpdate — é mesmo o OSU1?")
                    return
                total, done = sum(m.file_size for m in members), 0
                for m in members:
                    target = (dest / m.filename).resolve()
                    if dest not in target.parents:          # zip-slip: caminho saindo do destino
                        self.done.emit(False, f"entrada suspeita no zip: {m.filename}")
                        return
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(m) as src, open(target, "wb") as out:
                        while chunk := src.read(self.CHUNK):
                            if self.cancelled:
                                self.done.emit(False, "cancelado")
                                return
                            out.write(chunk)
                            done += len(chunk)
                            self.progress.emit(done, total, m.filename)
            self.done.emit(True, "")
        except (zipfile.BadZipFile, OSError) as e:
            self.done.emit(False, str(e))


class FirmwareRow(QFrame):
    """Linha da fila mostrando a instalação do firmware (extração do zip + validação no núcleo)."""

    def __init__(self, title: str, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)
        top = QHBoxLayout()
        self.name = QLabel(f"<b>Firmware do sistema</b>&nbsp;&nbsp;<span style='color:{FAINT}'>{title}</span>")
        self.info = QLabel("Preparando…")
        self.info.setStyleSheet(f"color:{MUTED}; font-size:12px; font-family:'JetBrains Mono','DejaVu Sans Mono',monospace;")
        self.cancel_btn = QPushButton("Cancelar")
        self.cancel_btn.setStyleSheet("padding:3px 10px; font-size:11px;")
        top.addWidget(self.name, 1)
        top.addWidget(self.info)
        top.addWidget(self.cancel_btn)
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.bar.setTextVisible(False)
        lay.addLayout(top)
        lay.addWidget(self.bar)

    def set_progress(self, done: int, total: int, text: str) -> None:
        if total > 0:
            self.bar.setRange(0, 1000)
            self.bar.setValue(int(1000 * done / total))
        else:
            self.bar.setRange(0, 0)                  # indeterminado
        self.info.setText(text)

    def finish(self, text: str, ok: bool = True) -> None:
        self.bar.setRange(0, 1000)
        self.bar.setValue(1000 if ok else 0)          # falha: barra vazia, texto em vermelho
        self.info.setText(text)
        if not ok:
            self.info.setStyleSheet(self.info.styleSheet() + "color:#D27373;")
        self.cancel_btn.hide()
        QTimer.singleShot(4000, self.deleteLater)


class CoreConsole(QDialog):
    def __init__(self, core: str, args: list[str], title: str, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"SeriesS-Emu — {title}")
        self.resize(1000, 600)
        lay = QVBoxLayout(self)
        self.out = QPlainTextEdit(readOnly=True)
        self.out.setMaximumBlockCount(30000)
        lay.addWidget(self.out)
        row = QHBoxLayout()
        self.status = QLabel("Iniciando núcleo…")
        self.status.setStyleSheet(f"color:{MUTED}")
        stop = QPushButton("Parar")
        row.addWidget(self.status, 1)
        row.addWidget(stop)
        lay.addLayout(row)
        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self._read)
        self.proc.finished.connect(lambda code, _s: self.status.setText(f"Núcleo encerrado (código {code})"))
        stop.clicked.connect(self.proc.kill)
        self.out.appendPlainText(f"$ {Path(core).name} {' '.join(args)}")
        self.proc.start(core, args)

    def _read(self) -> None:
        self.out.moveCursor(QTextCursor.MoveOperation.End)
        self.out.insertPlainText(bytes(self.proc.readAllStandardOutput()).decode("utf-8", "replace"))
        self.out.moveCursor(QTextCursor.MoveOperation.End)

    def closeEvent(self, e) -> None:  # noqa: N802
        if self.proc.state() != QProcess.ProcessState.NotRunning:
            self.proc.kill()
            self.proc.waitForFinished(1000)
        super().closeEvent(e)


# ---------------------------------------------------------------------------
#  Janela principal
# ---------------------------------------------------------------------------
class MainWindow(QMainWindow):
    def __init__(self, library: Library | None = None):
        super().__init__()
        uic.loadUi(str(UI_FILE), self)
        self.library = library or Library()
        self.core = find_core()

        mem_bar = MemoryBar(self.rail)                      # substitui o placeholder do .ui
        self.railLayout.replaceWidget(self.memBar, mem_bar)
        self.memBar.deleteLater()
        self.memBar = mem_bar
        self.memInfo.setText(f"{USER_SPACE_GB} GB título (limite rígido)  ·  {SYSTEM_GB} GB sistema")
        self.ssdBar.setRange(0, 1000)
        self.installQueue.setVisible(False)
        self.installCard.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Maximum)
        self.btnFirmware.clicked.connect(self.install_firmware)
        self._fw_busy = False
        self.update_firmware_status()
        self.coreStatus.setText(f"núcleo: {Path(self.core).name} ✓" if self.core
                                else "núcleo C++ não encontrado — compile com CMake")

        g = self.gameList
        g.setItemDelegate(GameRowDelegate(g))
        g.setMouseTracking(True)
        g.setVerticalScrollMode(QListWidget.ScrollMode.ScrollPerPixel)
        g.setUniformItemSizes(True)
        g.currentItemChanged.connect(lambda cur, _prev: self.show_details(cur))
        g.itemDoubleClicked.connect(lambda _i: self.launch())
        self.splitter.setStretchFactor(0, 1)
        self.splitter.setStretchFactor(1, 0)
        self.detailPanel.installEventFilter(self)    # esconde a capa quando falta altura

        self.btnInstall.clicked.connect(self.install_from_input)
        self.urlInput.returnPressed.connect(self.install_from_input)
        self.btnImport.clicked.connect(self.import_iso)
        self.btnLaunch.clicked.connect(self.launch)
        self.btnBench.clicked.connect(self.bench)
        self.btnRemove.clicked.connect(self.remove_selected)
        self.searchBox.textChanged.connect(lambda _t: self.refresh())
        for text, seq, slot in (("Instalar", "Ctrl+L", self.urlInput.setFocus), ("Importar", "Ctrl+O", self.import_iso),
                                ("Buscar", "Ctrl+F", self.searchBox.setFocus), ("Recarregar", "F5", self.reload),
                                ("Sair", "Ctrl+Q", self.close)):
            a = QAction(text, self)
            a.setShortcut(QKeySequence(seq))
            a.triggered.connect(slot)
            self.addAction(a)
        self.reload()

    def eventFilter(self, obj, event) -> bool:  # noqa: N802 (API Qt)
        if obj is self.detailPanel and event.type() == QEvent.Type.Resize:
            QTimer.singleShot(0, lambda: self.show_details(self.gameList.currentItem()))
        return super().eventFilter(obj, event)

    # ---------------- dados
    def reload(self) -> None:
        self.library.load()
        self.refresh()

    def refresh(self, select_id: str | None = None) -> None:
        cur = self.selected()
        select_id = select_id or (cur.id if cur else None)
        q = self.searchBox.text().strip().lower()
        games = sorted((g for g in self.library.games
                        if not q or q in g.title.lower() or q in " ".join(g.tags).lower() or q in g.title_id.lower()),
                       key=lambda g: g.added_at, reverse=True)
        lst = self.gameList
        lst.blockSignals(True)
        lst.clear()
        for game in games:
            it = QListWidgetItem()
            it.setData(Qt.ItemDataRole.UserRole, game)
            it.setSizeHint(QSize(400, GameRowDelegate.ROW_H))
            lst.addItem(it)
            if game.id == select_id:
                lst.setCurrentItem(it)
        if lst.currentItem() is None and lst.count():
            lst.setCurrentRow(0)
        lst.blockSignals(False)
        self.gameCount.setText(str(len(self.library.games)))
        self.listStack.setCurrentWidget(lst if games else self.emptyState)
        self.emptyState.setText("Nenhum resultado." if self.library.games else
                                "Nenhum jogo instalado.\nCole um link direto .iso/.xvc acima ou importe uma imagem.")
        used = self.library.used_bytes()
        self.ssdBar.setValue(int(1000 * min(1.0, used / SSD_CAPACITY)))
        self.ssdInfo.setText(f"{gb(used)} de 512 GB  ·  {gb(max(0, SSD_CAPACITY - used))} livres")
        self.show_details(lst.currentItem())

    def selected(self) -> Game | None:
        it = self.gameList.currentItem()
        return it.data(Qt.ItemDataRole.UserRole) if it else None

    # ---------------- detalhes
    def show_details(self, item) -> None:
        game: Game | None = item.data(Qt.ItemDataRole.UserRole) if item else None
        for b in (self.btnLaunch, self.btnRemove):
            b.setEnabled(game is not None)
        self.btnBench.setEnabled(bool(game and self.core and Path(game.path).is_file()
                                      and game.format.startswith("XDVDFS")))
        if game is None:
            self.detailCover.clear()
            self.detailCover.setVisible(False)
            self.detailTitle.setText("Selecione um jogo")
            for lbl in (self.detailPublisher, self.detailTags):
                lbl.setText("")
            for lbl in (self.detailResolution, self.detailFps, self.detailTitleId, self.detailSize,
                        self.detailFormat, self.detailSource):
                lbl.setText("—")
            return
        # Com a fila de instalação aberta pode faltar altura: a capa sai antes das especificações.
        self.detailCover.setVisible(self.detailPanel.height() >= 660)
        self.detailCover.setPixmap(cover_for(game, 120, 160, 8))
        self.detailTitle.setText(game.title)
        self.detailPublisher.setText(game.publisher or "Editora não informada")
        self.detailTags.setText("&nbsp;&nbsp;·&nbsp;&nbsp;".join(
            f"<span style='color:{ACCENT if t in HW_TAGS else MUTED}'>{t}</span>" for t in game.tags))
        up = " → 3840×2160" if game.target_height < 2160 else ""
        self.detailResolution.setText(f"{game.target_width}×{game.target_height}{up}")
        self.detailFps.setText(f"{game.target_fps}")
        self.detailTitleId.setText(game.title_id or "—")
        self.detailSize.setText(gb(game.size_bytes) if game.size_bytes else "—")
        self.detailFormat.setText(game.format or "—")
        self.detailSource.setText("Download (simulado)" if game.extra.get("mock") else
                                  "Download" if game.source == "download" else "Importado")

    # ---------------- instalar
    @staticmethod
    def validate_url(url: str) -> str | None:
        u = urlparse(url)
        if u.scheme not in ("http", "https") or not u.netloc:
            return "Informe um link http(s) completo."
        if not unquote(u.path).lower().endswith(ACCEPTED_EXTENSIONS):
            return "O link precisa apontar direto para um arquivo .iso ou .xvc."
        return None

    def install_from_input(self) -> None:
        url = self.urlInput.text().strip()
        if err := self.validate_url(url):
            self.statusbar.showMessage(err, 5000)
            self.urlInput.setFocus()
            return
        self.start_install(url)
        self.urlInput.clear()

    def start_install(self, url: str) -> InstallRow | None:
        size = install_size(url)
        pending = sum(r.total for r in self.installQueue.findChildren(InstallRow))
        if self.library.used_bytes() + pending + size > SSD_CAPACITY:
            QMessageBox.warning(self, "SSD cheio",
                                f"Este jogo precisa de {gb(size)} e o SSD de 512 GB não tem espaço livre suficiente.")
            return None
        row = InstallRow(url, self.library.root, self.installQueue)
        row.finished.connect(self.on_installed)
        row.cancelled.connect(lambda r: self.statusbar.showMessage(f"Instalação de {r.file_name} cancelada.", 4000))
        row.destroyed.connect(lambda *_: QTimer.singleShot(0, self._prune_queue))
        self.queueLayout.addWidget(row)
        self.installQueue.setVisible(True)
        self.statusbar.showMessage(f"Instalando {row.file_name}… (download simulado)")
        return row

    def _prune_queue(self) -> None:
        if not self.installQueue.findChildren(InstallRow):
            self.installQueue.setVisible(False)

    def on_installed(self, game: Game) -> None:
        self.library.load()
        self.library.upsert(game)
        self.library.save()
        self.refresh(select_id=game.id)
        self.statusbar.showMessage(f"{game.title} instalado.", 6000)

    # ---------------- importar / executar
    def import_iso(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Importar imagem", str(Path.home()),
                                              "Imagens (*.iso *.xiso *.xvc *.xvd);;Todos os arquivos (*)")
        if path:
            self.import_path(path)

    def import_path(self, path: str) -> bool:
        p = Path(path).resolve()
        gid = "local-" + hashlib.sha1(str(p).encode()).hexdigest()[:10]
        game = Game(id=gid, title=title_from_url(p.as_uri())[1], path=str(p), size_bytes=p.stat().st_size,
                    added_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), tags=list(HW_TAGS))
        if self.core:
            thumb = self.library.root / "thumbnails" / f"{gid}.png"
            proc = QProcess(self)
            proc.start(self.core, ["info", str(p), "--thumbnail", str(thumb)])
            proc.waitForFinished(30000)
            out = bytes(proc.readAllStandardOutput()).decode("utf-8", "replace").strip()
            try:
                meta = json.loads(out.splitlines()[-1])
            except (ValueError, IndexError):
                QMessageBox.critical(self, "Falha ao importar", out or "O núcleo não conseguiu ler a imagem.")
                return False
            if meta.get("format") == "Unknown":
                QMessageBox.warning(self, "Formato desconhecido",
                                    "A imagem não tem assinatura XDVDFS nem XVD.\n" + "\n".join(meta.get("warnings", [])))
                return False
            game.title = meta.get("title") or game.title
            game.title_id = meta.get("title_id", "")
            game.publisher = meta.get("publisher", "")
            game.format = meta.get("format", "")
            game.target_width = meta.get("target_width", 2560)
            game.target_height = meta.get("target_height", 1440)
            game.target_fps = meta.get("target_fps", 60)
            game.tags = meta.get("tags") or game.tags
            if thumb.is_file():
                game.thumbnail = str(thumb)
                _covers.clear()
        else:
            game.format = p.suffix.upper().lstrip(".")
        self.library.load()
        self.library.upsert(game)
        self.library.save()
        self.refresh(select_id=game.id)
        self.statusbar.showMessage(f"Importado: {game.title}", 5000)
        return True

    # ---------------- firmware (OSU1)
    def update_firmware_status(self) -> None:
        v = installed_firmware_version(self.library.root)
        self.fwInfo.setText(f"Firmware {v} instalado" if v else "Firmware: não instalado\n(o boot usa o kernel HLE)")
        self.btnFirmware.setText("Atualizar firmware (OSU1)" if v else "Instalar firmware (OSU1)")

    def install_firmware(self) -> None:
        if self._fw_busy or not self._need_core():
            return
        box = QMessageBox(self)
        box.setWindowTitle("Instalar firmware do sistema")
        box.setText("Selecione a Atualização Offline do Sistema (OSU1) baixada do site de suporte do Xbox.")
        box.setInformativeText("Pode ser o arquivo .zip ou a pasta já extraída que contém $SystemUpdate.\n\n"
                               "Os pacotes do sistema são criptografados pelo console: o emulador valida e registra "
                               "a versão, mas continua executando com o kernel HLE.")
        zip_btn = box.addButton("Arquivo .zip", QMessageBox.ButtonRole.AcceptRole)
        dir_btn = box.addButton("Pasta", QMessageBox.ButtonRole.AcceptRole)
        box.addButton("Cancelar", QMessageBox.ButtonRole.RejectRole)
        box.exec()
        if box.clickedButton() is zip_btn:
            path, _ = QFileDialog.getOpenFileName(self, "OSU1.zip", str(Path.home()), "Atualização offline (*.zip)")
            if path:
                self.install_firmware_zip(Path(path))
        elif box.clickedButton() is dir_btn:
            path = QFileDialog.getExistingDirectory(self, "Pasta do OSU1 ($SystemUpdate)", str(Path.home()))
            if path:
                self.install_firmware_dir(Path(path), move=False)

    def _firmware_row(self, title: str) -> FirmwareRow:
        row = FirmwareRow(title, self.installQueue)
        row.destroyed.connect(lambda *_: QTimer.singleShot(0, self._prune_queue))
        self.queueLayout.addWidget(row)
        self.installQueue.setVisible(True)
        return row

    def install_firmware_zip(self, zip_path: Path) -> None:
        root = firmware_root(self.library.root)
        incoming = root / "_incoming"
        shutil.rmtree(incoming, ignore_errors=True)
        incoming.mkdir(parents=True, exist_ok=True)
        self._fw_busy = True
        row = self._firmware_row(zip_path.name)
        ex = ZipExtractor(zip_path, incoming)
        self._extractor = ex                                    # mantém referência viva

        def on_progress(done: int, total: int, name: str) -> None:
            row.set_progress(done, total, f"Extraindo  {gb(done)} / {gb(total)}  {Path(name).name}")

        def on_done(ok: bool, err: str) -> None:
            if not ok:
                shutil.rmtree(incoming, ignore_errors=True)
                self._fw_busy = False
                row.finish("Falhou", ok=False)
                QMessageBox.warning(self, "Firmware não instalado", err)
                return
            self._run_fwinstall(incoming, move=True, row=row)

        ex.progress.connect(on_progress)
        ex.done.connect(on_done)
        row.cancel_btn.clicked.connect(lambda: setattr(ex, "cancelled", True))
        ex.start()

    def install_firmware_dir(self, folder: Path, move: bool) -> None:
        self._fw_busy = True
        row = self._firmware_row(folder.name)
        self._run_fwinstall(folder, move=move, row=row)

    def _run_fwinstall(self, source: Path, move: bool, row: FirmwareRow) -> None:
        row.set_progress(0, 0, "Validando pacotes XVD…")
        proc = QProcess(self)
        env = QProcessEnvironment.systemEnvironment()
        env.insert("XSS_HOME", str(self.library.root))           # mesma pasta que a UI usa
        proc.setProcessEnvironment(env)
        out: list[str] = []

        def on_output() -> None:
            text = bytes(proc.readAllStandardOutput()).decode("utf-8", "replace")
            for line in text.splitlines():
                out.append(line)
                if "copiando" in line:                           # progresso da cópia de uma pasta
                    row.set_progress(0, 0, line.split("] ", 2)[-1][:70])

        def on_finished(code: int, _status) -> None:
            self._fw_busy = False
            row.cancel_btn.hide()
            info = {}
            for line in reversed(out):
                if line.startswith("{"):
                    try:
                        info = json.loads(line)
                    except ValueError:
                        pass
                    break
            if move:
                shutil.rmtree(firmware_root(self.library.root) / "_incoming", ignore_errors=True)
            if code != 0 or not info.get("valid"):
                row.finish("Recusado", ok=False)
                QMessageBox.warning(self, "Firmware não instalado",
                                    info.get("error") or "\n".join(out[-5:]) or "o núcleo recusou a pasta")
                return
            n = info.get("system_packages", 0)
            row.finish(f"Instalado  {info.get('version')}  ·  {n} pacotes  ·  {gb(info.get('total_bytes', 0))}")
            self.update_firmware_status()
            self.statusbar.showMessage(f"Firmware {info.get('version')} instalado.", 6000)

        proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        proc.readyReadStandardOutput.connect(on_output)
        proc.finished.connect(on_finished)
        row.cancel_btn.clicked.connect(proc.kill)
        args = ["fwinstall", str(source)] + (["--move"] if move else [])
        proc.start(self.core, args)

    def _need_core(self) -> bool:
        if self.core:
            return True
        QMessageBox.information(self, "Núcleo não encontrado",
                                "Compile o núcleo C++:\n\ncmake -S . -B build\ncmake --build build --config Release")
        return False

    def launch(self) -> None:
        game = self.selected()
        if game and self._need_core():
            # Títulos comerciais não rodam (XVC criptografado); o boot executa a ROM de diagnóstico.
            CoreConsole(self.core, ["boot", "--max", "20000"], game.title, self).show()

    def bench(self) -> None:
        game = self.selected()
        if game and self._need_core():
            CoreConsole(self.core, ["bench", game.path], f"Benchmark de I/O — {game.title}", self).show()

    def remove_selected(self) -> None:
        game = self.selected()
        if not game:
            return
        if QMessageBox.question(self, "Remover", f"Remover {game.title} da biblioteca?\n"
                                "O arquivo de imagem não é apagado.") != QMessageBox.StandardButton.Yes:
            return
        self.library.load()
        self.library.remove(game.id)
        self.library.save()
        self.refresh()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("SeriesS-Emu")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)
    w = MainWindow()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
