#!/usr/bin/env python3
"""
XOne-Emu — interface gráfica (biblioteca de jogos).

Mistura de dashboard do Xbox com Steam: grade de capas, painel de detalhes,
importação de ISO (via núcleo C++) e "Baixar ISO" com download simulado.

A biblioteca é o MESMO library.json usado pelo núcleo (filesystem/content_store.cpp):
    $XONE_HOME/library.json   (padrão: ~/.xone-emu/library.json)

Executar:
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
import time
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote, urlparse

from PyQt6 import uic
from PyQt6.QtCore import (QEasingCurve, QEvent, QPropertyAnimation, QProcess, QRect, QRectF, QSize, Qt,
                          QTimer, pyqtSignal)
from PyQt6.QtGui import (QAction, QBrush, QColor, QFont, QFontMetrics, QIcon, QKeySequence,
                         QLinearGradient, QPainter, QPainterPath, QPen, QPixmap, QTextCursor)
from PyQt6.QtWidgets import (QApplication, QDialog, QFileDialog, QFrame, QGraphicsOpacityEffect,
                             QHBoxLayout, QInputDialog, QLabel, QLineEdit, QListWidget,
                             QListWidgetItem, QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar,
                             QPushButton, QStyle, QStyledItemDelegate, QStyleOptionViewItem,
                             QVBoxLayout, QWidget)

APP_DIR = Path(__file__).resolve().parent
PROJECT_DIR = APP_DIR.parent
UI_FILE = APP_DIR / "game_library.ui"

XBOX_GREEN = "#107C10"
XBOX_GREEN_HI = "#1DB954"
ACCEPTED_EXTENSIONS = (".iso", ".xvc", ".xiso", ".xvd")

CARD_W, CARD_H = 190, 300
COVER_H = 240

# ---------------------------------------------------------------------------
#  Tema escuro estilo Xbox
# ---------------------------------------------------------------------------
STYLESHEET = f"""
* {{ font-family: "Segoe UI", "Inter", "Noto Sans", sans-serif; color: #E6E6E6; }}
QMainWindow, #centralwidget, #content {{ background: #101010; }}
#sidebar {{ background: #171717; border-right: 1px solid #242424; }}
#logoLabel {{ font-size: 24px; font-weight: 800; color: {XBOX_GREEN_HI}; letter-spacing: 1px; }}
#logoSubLabel {{ color: #7A7A7A; font-size: 11px; }}
#sidebar QPushButton {{
    text-align: left; padding: 10px 12px; border: none; border-radius: 6px;
    background: transparent; font-size: 13px; color: #BDBDBD;
}}
#sidebar QPushButton:hover {{ background: #232323; color: white; }}
#sidebar QPushButton:checked {{ background: {XBOX_GREEN}; color: white; font-weight: 600; }}
#hardwareLabel {{ color: #8A8A8A; font-size: 11px; font-weight: 700; text-transform: uppercase; }}
#hardwareInfo {{ color: #9E9E9E; font-size: 11px; line-height: 150%; }}
#coreStatus {{ color: #7A7A7A; font-size: 11px; padding-top: 6px; }}
#pageTitle {{ font-size: 26px; font-weight: 700; color: white; }}
#gameCount {{ color: #8A8A8A; font-size: 13px; padding-top: 8px; }}
QLineEdit, QComboBox {{
    background: #1E1E1E; border: 1px solid #2E2E2E; border-radius: 6px; padding: 7px 10px;
    selection-background-color: {XBOX_GREEN};
}}
QLineEdit:focus, QComboBox:focus {{ border-color: {XBOX_GREEN}; }}
QComboBox QAbstractItemView {{ background: #1E1E1E; selection-background-color: {XBOX_GREEN}; }}
QPushButton {{
    background: #262626; border: 1px solid #333; border-radius: 6px; padding: 8px 16px; font-weight: 600;
}}
QPushButton:hover {{ background: #303030; border-color: #444; }}
QPushButton:disabled {{ color: #555; background: #1A1A1A; border-color: #222; }}
#btnDownload, #btnPlay {{ background: {XBOX_GREEN}; border: 1px solid {XBOX_GREEN}; color: white; }}
#btnDownload:hover, #btnPlay:hover {{ background: #138A13; }}
#btnPlay {{ padding: 12px; font-size: 15px; }}
#btnPlay:disabled {{ background: #1A2A1A; border-color: #1A2A1A; color: #4A5A4A; }}
#btnRemove {{ background: transparent; border: 1px solid #3A2222; color: #C46A6A; }}
#btnRemove:hover {{ background: #2A1616; }}
QListWidget#gameGrid {{ background: transparent; border: none; outline: none; }}
#emptyState {{ color: #6E6E6E; font-size: 15px; }}
#detailsPanel {{ background: #171717; border: 1px solid #242424; border-radius: 10px; }}
#detailTitle {{ font-size: 20px; font-weight: 700; color: white; }}
#detailPublisher {{ color: #8A8A8A; }}
#specsFrame {{ background: #1D1D1D; border-radius: 8px; padding: 8px; }}
#specsFrame QLabel {{ font-size: 12px; }}
#lblRes, #lblFps, #lblTid, #lblFmt, #lblSize, #lblSrc {{ color: #8A8A8A; }}
#detailPath {{ color: #5E5E5E; font-size: 10px; }}
#downloadsPanel {{ background: #161616; border: 1px solid #242424; border-radius: 10px; }}
#downloadsTitle {{ font-weight: 700; color: #BDBDBD; }}
QProgressBar {{
    background: #222; border: none; border-radius: 4px; height: 8px; text-align: center; font-size: 1px;
}}
QProgressBar::chunk {{
    border-radius: 4px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {XBOX_GREEN}, stop:1 {XBOX_GREEN_HI});
}}
QSplitter::handle {{ background: transparent; width: 14px; }}
QStatusBar {{ background: #0C0C0C; color: #7A7A7A; border-top: 1px solid #1E1E1E; }}
QScrollBar:vertical {{ background: transparent; width: 10px; }}
QScrollBar::handle:vertical {{ background: #2E2E2E; border-radius: 5px; min-height: 30px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QMessageBox, QInputDialog, QDialog {{ background: #1A1A1A; }}
QPlainTextEdit {{ background: #0A0A0A; border: 1px solid #222; font-family: "Cascadia Mono", "DejaVu Sans Mono", monospace; font-size: 11px; color: #9FE09F; }}
"""


# ---------------------------------------------------------------------------
#  Modelo: biblioteca compartilhada com o núcleo C++
# ---------------------------------------------------------------------------
def library_root() -> Path:
    if os.environ.get("XONE_HOME"):
        return Path(os.environ["XONE_HOME"])
    if sys.platform == "win32" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "XOne-Emu"
    return Path.home() / ".xone-emu"


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
    target_width: int = 1920
    target_height: int = 1080
    target_fps: int = 60
    extra: dict = field(default_factory=dict)  # chaves extras preservadas (ex.: "mock")

    KNOWN = ("id", "title", "title_id", "publisher", "path", "format", "thumbnail", "source",
             "added_at", "size_bytes", "target_width", "target_height", "target_fps")

    @classmethod
    def from_dict(cls, d: dict) -> "Game":
        known = {k: d[k] for k in cls.KNOWN if k in d}
        extra = {k: v for k, v in d.items() if k not in cls.KNOWN}
        return cls(**known, extra=extra)

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
            print(f"[XONE-UI] library.json inválido ({e}); cópia salva em {backup}", file=sys.stderr)
            self.games = []

    def save(self) -> None:
        tmp = self.file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps({"version": 1, "games": [g.to_dict() for g in self.games]},
                                  indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self.file)  # gravação atômica

    def upsert(self, game: Game) -> None:
        for i, g in enumerate(self.games):
            if g.id == game.id:
                self.games[i] = game
                return
        self.games.append(game)

    def remove(self, game_id: str) -> None:
        self.games = [g for g in self.games if g.id != game_id]


# ---------------------------------------------------------------------------
#  Ponte com o núcleo C++ (xone-emu)
# ---------------------------------------------------------------------------
def find_core_binary() -> str | None:
    exe = "xone-emu.exe" if sys.platform == "win32" else "xone-emu"
    candidates = [os.environ.get("XONE_CORE", "")]
    candidates += [str(PROJECT_DIR / "build" / sub / exe) for sub in ("", "Release", "Debug", "RelWithDebInfo")]
    for c in candidates:
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            return c
    return shutil.which(exe)


# ---------------------------------------------------------------------------
#  Capas
# ---------------------------------------------------------------------------
def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n} B"


def generated_cover(title: str, w: int, h: int) -> QPixmap:
    """Capa gerada (gradiente + título) para jogos sem thumbnail."""
    seed = int(hashlib.md5(title.encode()).hexdigest()[:8], 16)
    hue = seed % 360
    pm = QPixmap(w, h)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    g = QLinearGradient(0, 0, w * 0.4, h)
    g.setColorAt(0, QColor.fromHsv(hue, 170, 150))
    g.setColorAt(1, QColor.fromHsv((hue + 40) % 360, 200, 35))
    p.fillRect(0, 0, w, h, g)
    # anel decorativo estilo "orbe" do Xbox
    p.setPen(QPen(QColor(255, 255, 255, 40), max(2, w // 40)))
    r = int(w * 0.55)
    p.drawEllipse((w - r) // 2, int(h * 0.18), r, r)
    f = QFont()
    f.setBold(True)
    f.setPixelSize(max(12, w // 9))
    p.setFont(f)
    p.setPen(QColor(255, 255, 255, 235))
    p.drawText(QRect(12, int(h * 0.58), w - 24, int(h * 0.4)),
               Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop | Qt.TextFlag.TextWordWrap, title)
    p.end()
    return pm


_cover_cache: dict[tuple[str, str, int, int], QPixmap] = {}


def cover_for(game: Game, w: int, h: int) -> QPixmap:
    key = (game.id, game.thumbnail, w, h)
    if key in _cover_cache:
        return _cover_cache[key]
    pm = QPixmap(game.thumbnail) if game.thumbnail and Path(game.thumbnail).is_file() else QPixmap()
    if pm.isNull():
        pm = generated_cover(game.title, w, h)
    else:
        pm = pm.scaled(w, h, Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                       Qt.TransformationMode.SmoothTransformation)
        pm = pm.copy((pm.width() - w) // 2, (pm.height() - h) // 2, w, h)
    _cover_cache[key] = pm
    return pm


class GameCardDelegate(QStyledItemDelegate):
    """Desenha cada item da grade como um card com capa arredondada."""

    def sizeHint(self, option, index) -> QSize:  # noqa: N802 (API Qt)
        return QSize(CARD_W + 16, CARD_H + 16)

    def paint(self, p: QPainter, option: QStyleOptionViewItem, index) -> None:
        game: Game = index.data(Qt.ItemDataRole.UserRole)
        if game is None:
            return
        p.save()
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(option.rect).adjusted(8, 8, -8, -8)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hover = bool(option.state & QStyle.StateFlag.State_MouseOver)

        if hover or selected:  # leve "lift"
            r.translate(0, -3)

        card = QPainterPath()
        card.addRoundedRect(r, 10, 10)
        p.fillPath(card, QColor("#1C1C1C"))

        # capa com cantos superiores arredondados: recorta pelo contorno do card
        cover_rect = QRectF(r.x(), r.y(), r.width(), COVER_H)
        p.setClipPath(card)
        p.drawPixmap(cover_rect.toRect(), cover_for(game, int(r.width()), COVER_H))
        p.setClipping(False)

        # selo de origem
        if game.source == "download":
            badge = QRectF(r.right() - 74, r.y() + 8, 66, 20)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(16, 124, 16, 230))
            p.drawRoundedRect(badge, 4, 4)
            f = QFont(); f.setPixelSize(10); f.setBold(True)
            p.setFont(f); p.setPen(QColor("white"))
            p.drawText(badge, Qt.AlignmentFlag.AlignCenter, "BAIXADO")

        # título + linha secundária
        f = QFont(); f.setPixelSize(13); f.setBold(True)
        p.setFont(f); p.setPen(QColor("#F0F0F0"))
        text_rect = QRectF(r.x() + 10, r.y() + COVER_H + 8, r.width() - 20, 18)
        p.drawText(text_rect, Qt.AlignmentFlag.AlignLeft,
                   QFontMetrics(f).elidedText(game.title, Qt.TextElideMode.ElideRight, int(text_rect.width())))
        f2 = QFont(); f2.setPixelSize(11)
        p.setFont(f2); p.setPen(QColor("#8A8A8A"))
        sub = f"{game.target_height}p · {game.target_fps} FPS"
        if game.size_bytes:
            sub += f" · {human_size(game.size_bytes)}"
        p.drawText(QRectF(r.x() + 10, r.y() + COVER_H + 28, r.width() - 20, 16), Qt.AlignmentFlag.AlignLeft, sub)

        if selected or hover:
            p.setPen(QPen(QColor(XBOX_GREEN_HI if selected else "#3A3A3A"), 2.5 if selected else 1.5))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(r, 10, 10)
        p.restore()


# ---------------------------------------------------------------------------
#  Download (simulado)
# ---------------------------------------------------------------------------
def title_from_url(url: str) -> tuple[str, str]:
    name = unquote(Path(urlparse(url).path).name) or "jogo.iso"
    stem = Path(name).stem
    for ch in "._-+":
        stem = stem.replace(ch, " ")
    title = " ".join(w if w.isupper() else w.capitalize() for w in stem.split()) or "Jogo sem título"
    return name, title


class DownloadRow(QFrame):
    """Linha do painel de downloads com barra de progresso animada.

    O download é SIMULADO: nenhum byte é baixado da URL. O tamanho e a
    velocidade são sintéticos; ao concluir, o jogo é adicionado à biblioteca.
    """

    finished = pyqtSignal(object)   # Game
    cancelled = pyqtSignal(object)  # self

    TICK_MS = 50

    def __init__(self, url: str, library_root_dir: Path, parent=None):
        super().__init__(parent)
        self.url = url
        self.file_name, self.title = title_from_url(url)
        rnd = random.Random(url)
        self.total = rnd.randint(18, 60) * 1024 ** 3              # 18–60 GB, como jogos reais
        self.duration = rnd.uniform(6.0, 10.0)                     # segundos de simulação
        self.done = 0
        self.started = time.monotonic()
        self.dest = library_root_dir / "downloads" / self.file_name

        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 4, 0, 4)
        lay.setSpacing(4)
        top = QHBoxLayout()
        self.name_lbl = QLabel(f"<b>{self.title}</b> <span style='color:#777'>— {self.file_name}</span>")
        self.info_lbl = QLabel("Conectando…")
        self.info_lbl.setStyleSheet("color:#8A8A8A; font-size:11px;")
        self.cancel_btn = QPushButton("Cancelar")
        self.cancel_btn.setFixedHeight(26)
        self.cancel_btn.setStyleSheet("padding:2px 10px; font-size:11px;")
        self.cancel_btn.clicked.connect(self.cancel)
        top.addWidget(self.name_lbl, 1)
        top.addWidget(self.info_lbl)
        top.addWidget(self.cancel_btn)
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.bar.setFixedHeight(8)
        self.bar.setTextVisible(False)
        lay.addLayout(top)
        lay.addWidget(self.bar)

        self._anim = QPropertyAnimation(self.bar, b"value", self)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(self.TICK_MS)

    def _tick(self) -> None:
        elapsed = time.monotonic() - self.started
        if elapsed < 0.8:          # "handshake"
            return
        frac = min(1.0, (elapsed - 0.8) / self.duration)
        # velocidade com oscilação para parecer real
        frac = min(1.0, frac + 0.01 * random.uniform(-1, 1) * (1 - frac))
        self.done = max(self.done, int(self.total * frac))
        speed = self.total / self.duration * random.uniform(0.85, 1.15)
        remaining = (self.total - self.done) / speed if speed else 0
        self.info_lbl.setText(f"{human_size(self.done)} / {human_size(self.total)} · "
                              f"{human_size(int(speed))}/s · {remaining:0.0f}s restantes")
        self._anim.stop()
        self._anim.setStartValue(self.bar.value())
        self._anim.setEndValue(int(frac * 1000))
        self._anim.setDuration(self.TICK_MS * 3)
        self._anim.start()
        if frac >= 1.0:
            self._complete()

    def _complete(self) -> None:
        self._timer.stop()
        self._anim.stop()
        self.bar.setValue(1000)
        self.info_lbl.setText(f"Concluído · {human_size(self.total)}")
        self.cancel_btn.setEnabled(False)
        gid = "dl-" + hashlib.sha1(self.url.encode()).hexdigest()[:10]
        ext = Path(self.file_name).suffix.lower()
        game = Game(
            id=gid, title=self.title, path=str(self.dest),
            format="XVD/XVC" if ext in (".xvc", ".xvd") else "XDVDFS/GDF",
            source="download", added_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            size_bytes=self.total, target_width=1920, target_height=1080, target_fps=60,
            extra={"download_url": self.url, "mock": True},
        )
        self.finished.emit(game)
        QTimer.singleShot(4000, self._fade_out)

    def _fade_out(self) -> None:
        eff = QGraphicsOpacityEffect(self)
        self.setGraphicsEffect(eff)
        a = QPropertyAnimation(eff, b"opacity", self)
        a.setDuration(600)
        a.setStartValue(1.0)
        a.setEndValue(0.0)
        a.finished.connect(self.deleteLater)
        a.start()

    def cancel(self) -> None:
        self._timer.stop()
        self._anim.stop()
        self.info_lbl.setText("Cancelado")
        self.cancel_btn.setEnabled(False)
        self.cancelled.emit(self)
        QTimer.singleShot(1500, self.deleteLater)


# ---------------------------------------------------------------------------
#  Console do núcleo (saída do "Jogar")
# ---------------------------------------------------------------------------
class CoreConsole(QDialog):
    def __init__(self, core: str, args: list[str], title: str, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"XOne-Emu — {title}")
        self.resize(980, 560)
        lay = QVBoxLayout(self)
        self.out = QPlainTextEdit(readOnly=True)
        self.out.setMaximumBlockCount(20000)
        lay.addWidget(self.out)
        row = QHBoxLayout()
        self.status = QLabel("Iniciando núcleo…")
        stop = QPushButton("Parar")
        row.addWidget(self.status, 1)
        row.addWidget(stop)
        lay.addLayout(row)

        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self._read)
        self.proc.finished.connect(self._done)
        stop.clicked.connect(self.proc.kill)
        self.out.appendPlainText(f"$ {core} {' '.join(args)}")
        self.proc.start(core, args)

    def _read(self) -> None:
        text = bytes(self.proc.readAllStandardOutput()).decode("utf-8", "replace")
        self.out.moveCursor(QTextCursor.MoveOperation.End)
        self.out.insertPlainText(text)
        self.out.moveCursor(QTextCursor.MoveOperation.End)

    def _done(self, code: int, _status) -> None:
        self.status.setText(f"Núcleo encerrado (código {code})")

    def closeEvent(self, e) -> None:  # noqa: N802
        if self.proc.state() != QProcess.ProcessState.NotRunning:
            self.proc.kill()
            self.proc.waitForFinished(1000)
        super().closeEvent(e)


# ---------------------------------------------------------------------------
#  Janela principal
# ---------------------------------------------------------------------------
class MainWindow(QMainWindow):
    # widgets vindos do .ui (anotados para leitura)
    gameGrid: QListWidget
    searchBox: QLineEdit

    def __init__(self, library: Library | None = None):
        super().__init__()
        uic.loadUi(str(UI_FILE), self)
        self.library = library or Library()
        self.core = find_core_binary()
        self.view = "library"

        self._setup_grid()
        self._setup_actions()
        self.downloadsPanel.setVisible(False)
        self.coreStatus.setText(f"Núcleo: {Path(self.core).name} ✓" if self.core
                                else "Núcleo C++ não encontrado — compile com CMake (build/)")
        self.reload()
        self.statusbar.showMessage(f"Biblioteca: {self.library.file}", 6000)

    # ---------------- configuração
    def _setup_grid(self) -> None:
        g = self.gameGrid
        g.setViewMode(QListWidget.ViewMode.IconMode)
        g.setResizeMode(QListWidget.ResizeMode.Adjust)
        g.setMovement(QListWidget.Movement.Static)
        g.setUniformItemSizes(True)
        g.setSpacing(4)
        g.setMouseTracking(True)
        g.setVerticalScrollMode(QListWidget.ScrollMode.ScrollPerPixel)
        g.setItemDelegate(GameCardDelegate(g))
        g.currentItemChanged.connect(lambda cur, _prev: self.show_details(cur))
        g.itemDoubleClicked.connect(lambda _it: self.play())
        self.splitter.setStretchFactor(0, 1)
        self.splitter.setStretchFactor(1, 0)
        self.detailsPanel.installEventFilter(self)

    def eventFilter(self, obj, event) -> bool:  # noqa: N802 (API Qt)
        if obj is self.detailsPanel and event.type() == QEvent.Type.Resize:
            QTimer.singleShot(0, lambda: self.show_details(self.gameGrid.currentItem()))
        return super().eventFilter(obj, event)

    def _setup_actions(self) -> None:
        self.btnDownload.clicked.connect(self.ask_download)
        self.btnImport.clicked.connect(self.import_iso)
        self.btnPlay.clicked.connect(self.play)
        self.btnRemove.clicked.connect(self.remove_selected)
        self.searchBox.textChanged.connect(lambda _t: self.refresh_grid())
        self.sortBox.currentIndexChanged.connect(lambda _i: self.refresh_grid())
        for btn, view, title in ((self.navLibrary, "library", "Minha biblioteca"),
                                 (self.navRecent, "recent", "Adicionados recentemente"),
                                 (self.navDownloads, "downloads", "Downloads")):
            btn.clicked.connect(lambda _c=False, v=view, t=title: self.set_view(v, t))

        for text, seq, slot in (("Baixar ISO", "Ctrl+D", self.ask_download),
                                ("Importar ISO", "Ctrl+O", self.import_iso),
                                ("Buscar", "Ctrl+F", self.searchBox.setFocus),
                                ("Recarregar", "F5", self.reload),
                                ("Sair", "Ctrl+Q", self.close)):
            act = QAction(text, self)
            act.setShortcut(QKeySequence(seq))
            act.triggered.connect(slot)
            self.addAction(act)

    # ---------------- dados / grade
    def reload(self) -> None:
        self.library.load()
        self.refresh_grid()

    def set_view(self, view: str, title: str) -> None:
        self.view = view
        self.pageTitle.setText(title)
        for btn, v in ((self.navLibrary, "library"), (self.navRecent, "recent"), (self.navDownloads, "downloads")):
            btn.setChecked(v == view)
        self.refresh_grid()

    def visible_games(self) -> list[Game]:
        games = list(self.library.games)
        if self.view == "downloads":
            games = [g for g in games if g.source == "download"]
        q = self.searchBox.text().strip().lower()
        if q:
            games = [g for g in games if q in g.title.lower() or q in g.title_id.lower() or q in g.publisher.lower()]
        sort = self.sortBox.currentIndex()
        if self.view == "recent" or sort == 1:
            games.sort(key=lambda g: g.added_at, reverse=True)
            if self.view == "recent":
                games = games[:12]
        elif sort == 2:
            games.sort(key=lambda g: g.size_bytes, reverse=True)
        else:
            games.sort(key=lambda g: g.title.lower())
        return games

    def refresh_grid(self, select_id: str | None = None) -> None:
        current = self.selected_game()
        select_id = select_id or (current.id if current else None)
        g = self.gameGrid
        g.blockSignals(True)
        g.clear()
        games = self.visible_games()
        for game in games:
            it = QListWidgetItem()
            it.setData(Qt.ItemDataRole.UserRole, game)
            it.setSizeHint(QSize(CARD_W + 16, CARD_H + 16))
            it.setToolTip(f"{game.title}\n{game.publisher or ''}".strip())
            g.addItem(it)
            if game.id == select_id:
                g.setCurrentItem(it)
        g.blockSignals(False)
        n = len(self.library.games)
        self.gameCount.setText(f"{n} jogo" + ("" if n == 1 else "s"))
        self.gridStack.setCurrentWidget(self.gameGrid if games else self.emptyState)
        if not games and self.library.games:
            self.emptyState.setText("Nenhum jogo corresponde a este filtro.")
        elif not games:
            self.emptyState.setText("Sua biblioteca está vazia.\n\nUse “Importar ISO” para adicionar uma imagem do "
                                    "disco\nou “Baixar ISO” para colar um link direto (.iso / .xvc).")
        self.show_details(g.currentItem())

    def selected_game(self) -> Game | None:
        it = self.gameGrid.currentItem()
        return it.data(Qt.ItemDataRole.UserRole) if it else None

    # ---------------- detalhes
    def show_details(self, item: QListWidgetItem | None) -> None:
        game: Game | None = item.data(Qt.ItemDataRole.UserRole) if item else None
        self.btnPlay.setEnabled(game is not None)
        self.btnRemove.setEnabled(game is not None)
        if game is None:
            self.detailCover.setPixmap(QPixmap())
            self.detailTitle.setText("Selecione um jogo")
            self.detailPublisher.setText("")
            for lbl in (self.detailResolution, self.detailFps, self.detailTitleId, self.detailFormat,
                        self.detailSize, self.detailSource):
                lbl.setText("—")
            self.detailPath.setText("")
            return
        w = max(200, self.detailsPanel.width() - 36)
        # altura da capa se adapta ao espaço livre (ex.: quando o painel de downloads abre)
        h = max(140, min(300, self.detailsPanel.height() - 420))
        self.detailCover.setFixedHeight(h)
        cover = cover_for(game, int(h * 0.75), h)
        rounded = QPixmap(cover.size())
        rounded.fill(Qt.GlobalColor.transparent)
        p = QPainter(rounded)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        path = QPainterPath()
        path.addRoundedRect(QRectF(rounded.rect()), 10, 10)
        p.setClipPath(path)
        p.drawPixmap(0, 0, cover)
        p.end()
        self.detailCover.setPixmap(rounded)
        self.detailCover.setMinimumWidth(min(w, rounded.width()))
        self.detailTitle.setText(game.title)
        self.detailPublisher.setText(game.publisher or "Editora desconhecida")
        label_res = {2160: "4K", 1440: "1440p", 1080: "1080p", 900: "900p", 720: "720p"}.get(game.target_height, "")
        self.detailResolution.setText(f"{game.target_width} × {game.target_height} {label_res}".strip())
        self.detailFps.setText(f"{game.target_fps} FPS")
        self.detailTitleId.setText(game.title_id or "—")
        self.detailFormat.setText(game.format or "—")
        self.detailSize.setText(human_size(game.size_bytes) if game.size_bytes else "—")
        src = "Download (simulado)" if game.extra.get("mock") else ("Download" if game.source == "download" else "Importado")
        self.detailSource.setText(src)
        self.detailPath.setText(game.path)

    # ---------------- ações
    def ask_download(self) -> None:
        url, ok = QInputDialog.getText(self, "Baixar ISO",
                                       "Cole a URL de download direto (.iso ou .xvc):",
                                       QLineEdit.EchoMode.Normal, "https://")
        if not ok:
            return
        url = url.strip()
        err = self.validate_url(url)
        if err:
            QMessageBox.warning(self, "URL inválida", err)
            return
        self.start_download(url)

    @staticmethod
    def validate_url(url: str) -> str | None:
        u = urlparse(url)
        if u.scheme not in ("http", "https") or not u.netloc:
            return "Informe um link http(s) completo, por exemplo:\nhttps://exemplo.com/jogos/meu-jogo.iso"
        if not unquote(u.path).lower().endswith(ACCEPTED_EXTENSIONS):
            return f"O link precisa apontar diretamente para um arquivo {', '.join(ACCEPTED_EXTENSIONS)}."
        return None

    def start_download(self, url: str) -> DownloadRow:
        row = DownloadRow(url, self.library.root, self.downloadsPanel)
        row.finished.connect(self.on_download_finished)
        row.cancelled.connect(self._on_download_cancelled)
        # adiado: durante o sinal destroyed o filho ainda aparece em findChildren
        row.destroyed.connect(lambda *_: QTimer.singleShot(0, self._prune_downloads))
        self.downloadsLayout.addWidget(row)
        self.downloadsPanel.setVisible(True)
        self.statusbar.showMessage(f"Baixando {row.file_name}… (download simulado)")
        return row

    def on_download_finished(self, game: Game) -> None:
        self.library.load()            # pega alterações feitas pelo núcleo nesse meio-tempo
        self.library.upsert(game)
        self.library.save()
        self.refresh_grid(select_id=game.id)
        self.statusbar.showMessage(f"“{game.title}” adicionado à biblioteca.", 8000)

    def _on_download_cancelled(self, row: DownloadRow) -> None:
        self.statusbar.showMessage(f"Download de {row.file_name} cancelado.", 5000)

    def _prune_downloads(self) -> None:
        if not self.downloadsPanel.findChildren(DownloadRow):
            self.downloadsPanel.setVisible(False)

    def import_iso(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Importar imagem de jogo", str(Path.home()),
                                              "Imagens Xbox (*.iso *.xiso *.xvc *.xvd);;Todos os arquivos (*)")
        if path:
            self.import_path(path)

    def import_path(self, path: str) -> bool:
        if self.core:
            proc = QProcess(self)
            env = proc.processEnvironment()
            env.insert("XONE_HOME", str(self.library.root))
            proc.setProcessEnvironment(env)
            proc.start(self.core, ["import", path])
            proc.waitForFinished(30000)
            out = bytes(proc.readAllStandardOutput()).decode("utf-8", "replace").strip()
            err = bytes(proc.readAllStandardError()).decode("utf-8", "replace").strip()
            if proc.exitCode() != 0:
                QMessageBox.critical(self, "Falha ao importar", err or out or "Erro desconhecido no núcleo.")
                return False
            try:
                gid = json.loads(out.splitlines()[-1])["id"]
            except (ValueError, KeyError, IndexError):
                gid = None
            self.library.load()
            self.refresh_grid(select_id=gid)
            self.statusbar.showMessage(f"Importado via núcleo: {Path(path).name}", 6000)
            return True
        # Sem núcleo compilado: entrada básica (sem leitura de metadados da ISO)
        p = Path(path)
        game = Game(id="local-" + hashlib.sha1(str(p.resolve()).encode()).hexdigest()[:12],
                    title=title_from_url(p.as_uri())[1], path=str(p.resolve()),
                    format=p.suffix.upper().lstrip("."), size_bytes=p.stat().st_size,
                    added_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        self.library.upsert(game)
        self.library.save()
        self.refresh_grid(select_id=game.id)
        self.statusbar.showMessage("Importado sem metadados (núcleo C++ não compilado).", 6000)
        return True

    def play(self) -> None:
        game = self.selected_game()
        if not game:
            return
        if not self.core:
            QMessageBox.information(self, "Núcleo não encontrado",
                                    "Compile o núcleo C++ primeiro:\n\n"
                                    "cmake -S . -B build\ncmake --build build --config Release")
            return
        # O núcleo ainda não executa títulos comerciais (XVD criptografado / SO do console).
        # "Jogar" faz o boot UEFI e roda a ROM de diagnóstico, exibindo o trace da CPU.
        args = ["boot", "--max", "5000"]
        xbin = Path(game.path).with_suffix(".xbin")
        if xbin.is_file():
            args += ["--image", str(xbin)]
        CoreConsole(self.core, args, game.title, self).show()

    def remove_selected(self) -> None:
        game = self.selected_game()
        if not game:
            return
        if QMessageBox.question(self, "Remover", f"Remover “{game.title}” da biblioteca?\n"
                                "(O arquivo da imagem não é apagado.)") != QMessageBox.StandardButton.Yes:
            return
        self.library.load()
        self.library.remove(game.id)
        self.library.save()
        self.refresh_grid()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("XOne-Emu")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)
    w = MainWindow()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
