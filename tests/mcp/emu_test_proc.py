"""Úklid procesů a volné TCP porty pro e2e testy, které spouštějí emulátor.

Sdílený pomocník testů v ``tests/mcp`` (žádný test sám o sobě). Řeší dvě
vady, které se v praxi objevily:

1. **Osiřelé procesy emulátoru.** Když test selhal mimo ``try/finally``,
   byl přerušen (Ctrl+C) nebo ho ctest po ``TIMEOUT`` zabil, spuštěný
   emulátor běžel dál (nalezeny např. ``mz700emu-pal.exe
   --mcp-tcp-port=23879 --run-mzf ...``). ``install()`` proto:

   - na Windows zařadí *vlastní* proces testu do Job Objectu s
     ``JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`` (přes ``ctypes``, tedy jen
     standardní knihovna). Všichni potomci vytvoření potom (emulátor,
     venv interpret, ``mcp_server.py`` i jím spuštěný emulátor) jsou
     v jobu také. Jakmile proces testu skončí jakkoli (i
     ``TerminateProcess`` od ctestu), zavře se poslední handle jobu
     a systém ukončí všechny jeho zbylé procesy. Zasáhne to výhradně
     potomky testu, nikdy cizí instance emulátoru;
   - registruje každý ``subprocess.Popen`` vytvořený v procesu testu
     a při ``atexit`` (normální konec, výjimka, Ctrl+C, SIGTERM na POSIX)
     ukončí ty, které ještě běží - jen podle vlastních PID;
   - volitelně nastaví vnitřní limit doby běhu (``deadline_s``) kratší
     než ``TIMEOUT`` v ctestu: po jeho vypršení vypíše chybu, ukončí
     registrované potomky a proces skončí kódem 1. Úklid tak proběhne
     i tam, kde Job Object není (POSIX), dřív než ctest proces zabije.

2. **Kolize pevných TCP portů** při souběžných bězích testů.
   ``free_tcp_port()`` vrátí port, který právě přidělil systém
   (``bind`` na port 0), nikdy ne vyhrazený 23800.

Výstupy pro uživatele jsou anglicky.
"""

import atexit
import functools
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

#: Port vyhrazený pro běžné (ne testovací) instance emulátoru.
RESERVED_MCP_PORT = 23800

#: Seznam ``Popen`` objektů spuštěných v tomto procesu (jen vlastní PID).
_children = []
#: Zámek nad ``_children`` (Popen lze vytvářet z více vláken).
_children_lock = threading.Lock()
#: Handle Job Objectu (Windows); drží se po celou dobu života procesu.
_job_handle = None
#: True, pokud se vlastní proces podařilo zařadit do jobu.
_job_self = False
#: Ochrana proti opakované instalaci.
_installed = False


def _win_create_kill_on_close_job():
    """Vytvoří Job Object s ``KILL_ON_JOB_CLOSE`` (jen Windows).

    @return Dvojice (kernel32, handle) nebo (None, None) při chybě.
    @note Handle se vytváří bez dědičnosti, takže ho potomci nedrží
          a job zanikne s posledním handle v procesu testu.
    """
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
                    ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    JobObjectExtendedLimitInformation = 9
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000

    k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                            ctypes.c_void_p, wintypes.DWORD]
    k32.SetInformationJobObject.restype = wintypes.BOOL
    k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    k32.AssignProcessToJobObject.restype = wintypes.BOOL
    k32.GetCurrentProcess.restype = wintypes.HANDLE

    job = k32.CreateJobObjectW(None, None)
    if not job:
        return None, None
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not k32.SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       ctypes.byref(info), ctypes.sizeof(info)):
        return None, None
    return k32, job


def _win_assign(k32, job, proc_handle):
    """Zařadí proces (handle) do jobu; vrací True při úspěchu."""
    try:
        return bool(k32.AssignProcessToJobObject(job, proc_handle))
    except Exception:  # noqa: BLE001 - pojistka, nesmí shodit test
        return False


class _TrackedPopen(subprocess.Popen):
    """``subprocess.Popen``, který se zaregistruje k úklidu při konci testu.

    Pokud se vlastní proces testu nepodařilo zařadit do Job Objectu (např.
    starý Windows bez vnořených jobů), zařadí se do něj aspoň každý potomek
    hned po spuštění.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        with _children_lock:
            _children.append(self)
        if _job_handle is not None and not _job_self:
            _win_assign(_job_handle[0], _job_handle[1], int(self._handle))


def kill_children(timeout=5.0):
    """Ukončí všechny ještě běžící potomky spuštěné tímto procesem.

    Nejdřív ``terminate()``, po ``timeout`` sekundách ``kill()``. Zasahuje
    jen procesy z vlastního registru (vlastní PID), nikdy podle jména.

    @param timeout Čekání na dobrovolné ukončení po ``terminate()`` (s).
    @post Žádný registrovaný potomek neběží (pokud ho systém dovolil
          ukončit).
    """
    with _children_lock:
        procs = list(_children)
    alive = [p for p in procs if p.poll() is None]
    for p in alive:
        try:
            p.terminate()
        except OSError:
            pass
    for p in alive:
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                p.kill()
                p.wait(timeout=timeout)
            except (OSError, subprocess.TimeoutExpired):
                pass
        except OSError:
            pass


def kill_proc(proc, timeout=10.0):
    """Zabije jeden vlastní proces (je-li ještě živý) a počká na něj.

    @param proc Objekt ``subprocess.Popen`` spuštěný tímto testem.
    @param timeout Čekání na ukončení po ``kill()`` (s).
    """
    if proc is None:
        return
    try:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        pass


def kill_on_init_failure(init):
    """Dekorátor ``__init__`` obalu procesu: při výjimce zabij spuštěný proces.

    Obaly emulátoru v testech spustí proces v konstruktoru a pak čekají na
    hello nebo spojení. Když tahle část selže, volající objekt nedostane
    a jeho ``close()`` se nezavolá - proces by osiřel. Dekorátor proto při
    jakékoli výjimce z ``__init__`` zabije ``self.proc`` (nebo
    ``self.process``), zavře otevřené ``sock``/``f``/``log``/``_stderr``
    a výjimku propustí dál.
    """
    @functools.wraps(init)
    def wrapper(self, *args, **kwargs):
        try:
            init(self, *args, **kwargs)
        except BaseException:
            kill_proc(getattr(self, "proc", None) or getattr(self, "process", None))
            for attr in ("f", "sock", "log", "_stderr"):
                obj = getattr(self, attr, None)
                if obj is not None:
                    try:
                        obj.close()
                    except Exception:  # noqa: BLE001 - úklid nesmí zakrýt příčinu
                        pass
            raise
    return wrapper


def rmtree_retry(path, timeout=10.0):
    """Smaže adresář; když ho drží doznívající potomek, zkouší to znovu.

    Na Windows nejde smazat adresář, který je pracovním adresářem běžícího
    procesu nebo v němž je otevřený soubor. Proces spuštěný nepřímo (např.
    emulátor spuštěný ``mcp_server.py`` s ``cwd`` v dočasném adresáři) může
    po ukončení přímého potomka ještě chvíli doznívat; jednorázové
    ``shutil.rmtree(..., ignore_errors=True)`` pak tiše nechá prázdný
    adresář v ``tmp``.

    @param path Mazaný adresář (neexistující = nic se neděje).
    @param timeout Nejdelší doba opakování (s).
    @return True, pokud adresář po návratu neexistuje.
    """
    deadline = time.monotonic() + timeout
    while True:
        shutil.rmtree(path, ignore_errors=True)
        if not os.path.exists(path):
            return True
        if time.monotonic() >= deadline:
            print(f"WARNING: cannot remove temporary directory {path}",
                  file=sys.stderr)
            return False
        time.sleep(0.2)


def _deadline_expired(deadline_s):
    """Vnitřní limit doby běhu vypršel: ukonči potomky a skonči kódem 1."""
    print(f"ERROR: test exceeded its internal deadline of {deadline_s:.0f} s "
          f"(terminating spawned processes)", file=sys.stderr, flush=True)
    kill_children(timeout=3.0)
    os._exit(1)


def _sigterm_handler(signum, _frame):
    """SIGTERM/SIGHUP (POSIX): převeď na normální konec, aby proběhl atexit."""
    sys.exit(128 + signum)


def install(deadline_s=None):
    """Zapne úklid potomků pro tento proces testu (idempotentní).

    Volat co nejdřív, před spuštěním prvního procesu (a před případným
    znovuspuštěním testu pod venv interpretem - pak je v jobu i ten).

    @param deadline_s Vnitřní limit doby běhu v sekundách, nebo None.
           Má být kratší než ``TIMEOUT`` testu v ctestu, aby úklid i chybové
           hlášení proběhly dřív, než ctest proces zabije.
    @post ``subprocess.Popen`` v tomto procesu je nahrazen registrující
          podtřídou; na Windows je proces (nebo aspoň každý potomek)
          v Job Objectu s ``KILL_ON_JOB_CLOSE``; ``atexit`` ukončí běžící
          potomky.
    @note Selhání Job Objectu není chyba testu - zůstane registr + atexit.
    """
    global _installed, _job_handle, _job_self
    if _installed:
        return
    _installed = True

    if os.name == "nt":
        try:
            k32, job = _win_create_kill_on_close_job()
            if job:
                _job_handle = (k32, job)
                _job_self = _win_assign(k32, job, k32.GetCurrentProcess())
        except Exception:  # noqa: BLE001 - bez jobu zůstane registr + atexit
            _job_handle = None
    else:
        for sig in (signal.SIGTERM, signal.SIGHUP):
            try:
                signal.signal(sig, _sigterm_handler)
            except (ValueError, OSError):
                pass  # ne z hlavního vlákna - zůstane atexit

    subprocess.Popen = _TrackedPopen
    atexit.register(kill_children)

    if deadline_s:
        t = threading.Timer(float(deadline_s), _deadline_expired, args=(deadline_s,))
        t.daemon = True
        t.start()


def free_tcp_port(host="127.0.0.1"):
    """Vrátí TCP port, který je právě volný (přidělí ho systém přes port 0).

    Mezi uvolněním portu zde a jeho obsazením emulátorem je krátké okno,
    kdy ho může obsadit jiný proces (malé, ale nenulové riziko). Proti
    pevnému portu ale odpadá jistá kolize dvou souběžných běhů a připojení
    k osiřelému emulátoru z dřívějšího běhu.

    @param host Adresa, na které se port ověřuje (MCP TCP poslouchá na
           loopbacku).
    @return Číslo portu, nikdy ``RESERVED_MCP_PORT``.
    """
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind((host, 0))
            port = s.getsockname()[1]
        if port != RESERVED_MCP_PORT:
            return port


def mcp_test_port(env_var):
    """Ručně zvolený port MCP TCP z proměnné ``env_var``, nebo None.

    None znamená, že si volající pro každý spuštěný emulátor vezme volný
    port přes ``free_tcp_port()``.

    @param env_var Jméno proměnné prostředí s ručně zvoleným portem.
    @return Číslo portu, nebo None, když proměnná není nastavená.
    @exception SystemExit kódem 1, pokud proměnná určuje vyhrazený 23800.
    """
    val = os.environ.get(env_var)
    if not val:
        return None
    port = int(val)
    if port == RESERVED_MCP_PORT:
        print(f"ERROR: port {RESERVED_MCP_PORT} is reserved for regular instances",
              file=sys.stderr)
        sys.exit(1)
    return port
