#!/usr/bin/env python3
"""E2E test: čtení zachyceného zvuku po přepsání PCM vrací jen vzorky zvoleného okna (MCP pipe).

Paměť PCM jen 1 s (44 100 vzorků v každém proudu), takže při běhu
230 snímků ring přepíše začátek zachycení. Test ověřuje na skutečném
emulátoru (mz800emu = PAL, mz700emu-ntsc = NTSC, souběžně), že:

  1  Výřez `audio_capture_pcm` podle snímku (`frame_from`) při 100, 50
     a 200 %, proud reference i SDL, pro snímek zcela přepsaný, částečně
     přepsaný a plně zachovaný (a jejich spojení): vrácené bajty jsou
     přesně vzorky, které z intervalu snímku podle hranic (`audio_frames`
     ref_first / sdl_first) a prvního drženého indexu zbývají; nic
     z dalšího snímku. Sousední snímky na sebe navazují bez opakování
     a jejich spojení je totéž co společný dotaz. `gap` leží uvnitř
     požadovaného intervalu, `frame_range.audio_status` rozliší hranice
     a zvuk. Stránkování: částečný snímek se stropem `max_bytes` a
     pokračováním přes `sample_from` = `next_sample` dá tytéž bajty.
     Na f9da3035 vracel částečně přepsaný snímek 172 (100 %, MZ-800)
     882 vzorků od 152443, posledních 850 ze snímku 173.
  2  `audio_capture_summary` při 100 % po ručním i automatickém konci
     (`frames` 171): přepsaný snímek nedá "ok", částečný překryv se
     vyhodnotí právě na společných vzorcích obou proudů (`from_sample`,
     `compared_samples`), s `lag_frames` 0 i 1. Na f9da3035 dal zcela
     přepsaný snímek 118 "ok" s 881 vzorky od 106612 a společných 32 vzorků
     snímku 173 hlásil jako "no_overlap". `worst_frames` u jednoho snímku
     obsahuje právě ten snímek (dvojice [snímek, diff_rms]) a
     `worst_frames_detail` jeho počet porovnaných dvojic, i když je
     zachovaný jen částečně (na 1a6773d4 prázdný seznam). Hledání posunu
     (bez `lag_frames`) na okraji paměti po ručním zastavení: reference drží
     konec snímku, výstup už jen indexy za ním -> "no_overlap" s vlastním
     důvodem, `fixed_lag_min_samples` a `fixed_lag_min_frames`, a pevný posun
     z odpovědi porovná očekávané dvojice (na 1a6773d4 obecný důvod
     "output path", MZ-800 snímek 172 a MZ-700 NTSC snímek 163).
  3  Živé čtení: zachycení běží (async `run`, 100 % a MAX SPEED), dotazy na snímky
     u okraje přepisu: každá odpověď vrátí přesně průnik intervalu snímku
     s drženým úsekem `available` z téže odpovědi (kopie a průnik pod
     jedním zámkem), hranice odpovídají `audio_frames` po zastavení a obsah
     plně zachovaného snímku je po zastavení stejný. Délka živého čtení se
     řídí tím, co se zastihlo (aspoň 40 odpovědí a okno complete, partial
     i lost, 2 až 20 s), ne rychlostí stroje. Nejnovější snímky pro kontrolu
     obsahu se čtou při 100 % a hned se zastaví (při MAX SPEED by zpoždění
     zastavení 50 ms pod zátěží přepsalo celou paměť 1 s - rev37); když
     žádný nezůstal celý, zkusí se to znovu (nejvýš 3krát). Obsah se po
     zastavení porovná u všech živě celých snímků, které paměť drží;
     každý různý nejnovější snímek držený při zastavení se musí ověřit
     (počty odpovědí a různých výřezů se vedou zvlášť - tentýž snímek se
     mezi nejnovějšími může objevit dvakrát, posudek 1a0f1a18) a celý
     snímek přečtený živě víckrát musí mít ve všech čteních stejný obsah.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import base64
import hashlib
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, _find_exe  # noqa: E402
from test_audio_pcm_frame_index import _setup  # noqa: E402
from test_audio_read import read_rows  # noqa: E402

_SOURCES = (("reference", "ref_first"), ("sdl", "sdl_first"))


def _pcm(emu, source, max_bytes=4194304, **sel):
    """PCM 44100 Hz stereo; vrátí (odpověď, bajty) a ověří délku přenosu."""
    a = emu.call("audio_capture_pcm", dict(source=source, rate=44100, stereo=True, encoding="pcm_b64",
                                           max_bytes=max_bytes, **sel))
    raw = base64.b64decode(a["data_b64"])
    if len(raw) != 4 * a["samples"]:
        raise TestFailure(f"{source} {sel}: {len(raw)} bytes for {a['samples']} samples")
    return a, raw


def _direct(emu, source, lo, n):
    """Přímo vzorky [lo, lo + n) kurzorem (jen uvnitř drženého úseku)."""
    if n == 0:
        return b""
    a, raw = _pcm(emu, source, sample_from=lo, samples=n)
    if a["from_sample"] != lo or a["samples"] != n:
        raise TestFailure(f"{source}: direct read [{lo}, {lo + n}) returned [{a['from_sample']}, {a['next_sample']})")
    return raw


def _expect(starts, i, n, oldest, end):
    """Zbývající vzorky snímků [i, i + n): (začátek, počet)."""
    first, limit = starts[i], starts[i + n]
    lo = min(end, max(first, oldest))
    return lo, max(0, min(limit, end) - lo)


def _check_frames(emu, label, source, frames, starts, out):
    """Případ 1 pro jeden proud; vrátí True = prošlo."""
    st, _ = _pcm(emu, source, sample_from=0, samples=1)
    oldest, end = st["available"]
    pos = next((i for i in range(1, len(starts) - 3) if starts[i] < oldest < starts[i + 1]), None)
    if pos is None:
        raise TestFailure(f"{label} {source}: no partially overwritten frame (oldest {oldest})")
    ok = True
    got = {}
    for i, n in ((pos - 1, 1), (pos, 1), (pos + 1, 1), (pos, 2), (pos - 1, 3)):
        lo, cnt = _expect(starts, i, n, oldest, end)
        a, raw = _pcm(emu, source, frame_from=frames[i], frames=n)
        fr = a.get("frame_range") or {}
        want = _direct(emu, source, lo, cnt)
        gap = a.get("gap")
        want_gap = [starts[i], min(oldest, starts[i + n])] if starts[i] < oldest else None
        have_gap = [gap["from_sample"], gap["to_sample"]] if gap else None
        audio = "complete" if cnt == starts[i + n] - starts[i] else ("lost" if cnt == 0 else "partial")
        good = (raw == want and (cnt == 0 or a["from_sample"] == lo) and a["samples"] == cnt
                and not a["truncated"] and have_gap == want_gap
                and fr.get("status") == "complete" and fr.get("audio_status") == audio
                and fr.get("samples_requested") == [starts[i], starts[i + n]]
                and (cnt == 0 or fr.get("samples_held") == [lo, lo + cnt]))
        got[(i, n)] = (a, raw)
        ok = ok and good
        out.append(f"  {'PASS' if good else 'FAIL'}: {label} {source} frames [{frames[i]}, {frames[i] + n}) "
                   f"bounds [{starts[i]}, {starts[i + n]}) held from {oldest}: expected {cnt} samples from {lo}, "
                   f"got {a['samples']} from {a['from_sample']}, gap {have_gap} (expected {want_gap}), "
                   f"audio {fr.get('audio_status')} (expected {audio})")
    # spojení sousedů = společný dotaz, žádný vzorek dvakrát
    singles = [got[(pos + k, 1)] for k in (-1, 0, 1)]
    joined = b"".join(r for _, r in singles)
    seq = [(a["from_sample"], a["next_sample"]) for a, r in singles if r]
    no_repeat = all(seq[k][1] == seq[k + 1][0] for k in range(len(seq) - 1))
    good = joined == got[(pos - 1, 3)][1] and no_repeat
    ok = ok and good
    out.append(f"  {'PASS' if good else 'FAIL'}: {label} {source} frames {frames[pos - 1]}..{frames[pos + 1]} "
               f"one by one = one query, ranges {seq}")
    # stránkování částečného snímku se stropem max_bytes
    lo, cnt = _expect(starts, pos, 2, oldest, end)
    page = max(1024, 4 * max(1, cnt // 3))
    a, raw = _pcm(emu, source, max_bytes=page, frame_from=frames[pos], frames=2)
    pages, data = 1, raw
    truncated_first = a["truncated"]
    while a["truncated"] and a["next_sample"] < starts[pos + 2]:
        nxt = a["next_sample"]
        a, raw = _pcm(emu, source, max_bytes=page, sample_from=nxt, samples=starts[pos + 2] - nxt)
        pages += 1
        data += raw
    good = data == _direct(emu, source, lo, cnt) and truncated_first and pages >= 2
    ok = ok and good
    out.append(f"  {'PASS' if good else 'FAIL'}: {label} {source} frames [{frames[pos]}, {frames[pos] + 2}) "
               f"in {pages} pages of {page} B: {len(data) // 4} samples (expected {cnt})")
    return ok


def run_frames(exe, speed, out):
    """Případ 1 pro jednu rychlost; vrátí True = prošlo."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmow-"))
    emu = PipeEmu(exe, tmp)
    label = f"{exe.stem} {speed} %"
    try:
        _setup(emu, speed)
        emu.call("audio_capture_start", dict(events=False, psg_writes=False, reference_wav=False, sdl_wav=False,
                                             memory_minutes=1, pcm=["reference", "sdl"], pcm_seconds=1))
        emu.call("run", {"frames": 230}, timeout=120.0)
        emu.call("audio_capture_stop")
        fields, rows, _ = read_rows(emu, "audio_frames", ["seq", "ref_first", "sdl_first"])
        ok = True
        for source, col in _SOURCES:
            ok = _check_frames(emu, label, source, [r[fields.index("seq")] for r in rows],
                               [r[fields.index(col)] for r in rows], out) and ok
        return ok
    except Exception as e:  # noqa: BLE001 - výsledek vlákna (i chybějící pole odpovědi)
        out.append(f"  FAIL: {label}: {e!r}")
        return False
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def run_summary(exe, limit, out):
    """Případ 2: ruční (limit 0) nebo automatický konec; vrátí True = prošlo."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmsum-"))
    emu = PipeEmu(exe, tmp)
    label = f"{exe.stem} {'manual stop' if not limit else f'automatic end after {limit} frames'}"
    try:
        _setup(emu, 100)
        opts = dict(events=False, psg_writes=False, reference_wav=False, sdl_wav=False,
                    pcm=["reference", "sdl"], pcm_seconds=1)
        if limit:
            opts["frames"] = limit
        emu.call("audio_capture_start", opts)
        emu.call("run", {"frames": 230}, timeout=120.0)
        if not limit:
            emu.call("audio_capture_stop")
        fields, rows, _ = read_rows(emu, "audio_frames", ["seq", "t"])
        meta = emu.call("audio_capture_summary", {"frame_from": len(rows) - 2, "frame_to": len(rows) - 2,
                                                  "lag_frames": 0})
        clock = meta["real_clk_hz"]
        lag1 = 44100 * (meta["gdg_clk_hz"] // meta["fps"]) // clock   # _ac_frames_to_samples(1)
        ticks = [q[fields.index("t")] for q in rows]
        starts = [(t - ticks[0]) * 44100 // clock for t in ticks]
        held = {}
        for s, _ in _SOURCES:
            a, _ = _pcm(emu, s, sample_from=0, samples=1)
            held[s] = a["available"]
        i = next(i for i in range(len(starts) - 1) if starts[i] <= max(held["reference"][0], held["sdl"][0]) < starts[i + 1])
        ok = True
        for lag_frames, lag in ((0, 0), (1, lag1)):
            for f in range(i - 2, i + 3):
                q = emu.call("audio_capture_summary", {"frame_from": f, "frame_to": f, "lag_frames": lag_frames})
                c = q["ref_vs_sdl"]
                start = max(starts[f], held["reference"][0], held["sdl"][0] - lag)
                stop = min(starts[f + 1], held["reference"][1], held["sdl"][1] - lag)
                count = max(0, stop - start)
                status = c.get("status")
                # worst_frames: jediný snímek okna se hodnotí z celého porovnaného úseku (i částečný)
                # (dvojice [snímek, diff_rms] jako ve vydané verzi, počet dvojic ve worst_frames_detail)
                worst = c.get("worst_frames", [])
                det = c.get("worst_frames_detail", [])
                worst_ok = count == 0 or (len(worst) == 1 and len(worst[0]) == 2 and worst[0][0] == f
                                          and worst[0][1] == c["diff_rms"] and len(det) == 1
                                          and det[0]["frame"] == f and det[0]["compared_samples"] == count)
                good = ((status == "ok") == (count > 0) and (status == "ok" or status == "no_overlap")
                        and c.get("compared_samples", 0) == count
                        and (count == 0 or (c["from_sample"] == start and c["lag_samples"] == lag))
                        and (count > 0 or "diff_rms" not in c)
                        and c.get("window") == [starts[f], starts[f + 1]] and worst_ok)
                ok = ok and good
                out.append(f"  {'PASS' if good else 'FAIL'}: {label} summary frame {f} lag_frames={lag_frames}: "
                           f"window [{starts[f]}, {starts[f + 1]}), held ref {held['reference']} sdl {held['sdl']}: "
                           f"expected {count} pairs from {start}, got {status} {c.get('compared_samples')} "
                           f"from {c.get('from_sample')}, worst_frames {worst}")
        # Hledání posunu na okraji paměti: reference drží část okna, výstup až indexy za ní.
        # Posun se neodhaduje (no_overlap s vlastním důvodem) a nejmenší pevný posun
        # z odpovědi opravdu porovná dvojice.
        hits = 0
        for f in range(i - 2, i + 3):
            c = emu.call("audio_capture_summary", {"frame_from": f, "frame_to": f})["ref_vs_sdl"]
            ref_lo = max(starts[f], held["reference"][0])
            ref_hi = min(starts[f + 1], held["reference"][1])
            sdl0, sdl1 = held["sdl"]
            pairs0 = max(0, min(ref_hi, sdl1) - max(ref_lo, sdl0))
            edge = ref_lo < ref_hi and sdl0 >= ref_hi
            detail = ""
            if pairs0 > 0:
                good = c.get("status") == "ok" and "fixed_lag_min_samples" not in c
            elif not edge:
                good = c.get("status") == "no_overlap" and "fixed_lag_min_samples" not in c
            else:
                hits += 1
                min_lag = sdl0 - (ref_hi - 1)
                frames_k = next((k for k in range(1, 61)
                                 if 44100 * k * (meta["gdg_clk_hz"] // meta["fps"]) // clock >= min_lag
                                 and 44100 * k * (meta["gdg_clk_hz"] // meta["fps"]) // clock < sdl1 - ref_lo), None)
                good = (c.get("status") == "no_overlap" and "lag_samples" not in c
                        and "delay search" in c.get("reason", "")
                        and c.get("fixed_lag_min_samples") == min_lag and c.get("fixed_lag_min_frames") == frames_k)
                if frames_k is not None:
                    lag_k = 44100 * frames_k * (meta["gdg_clk_hz"] // meta["fps"]) // clock
                    d = emu.call("audio_capture_summary", {"frame_from": f, "frame_to": f,
                                                           "lag_frames": frames_k})["ref_vs_sdl"]
                    want_from = max(ref_lo, sdl0 - lag_k)
                    want_n = max(0, min(ref_hi, sdl1 - lag_k) - want_from)
                    good = (good and d.get("status") == "ok" and want_n > 0
                            and d.get("from_sample") == want_from and d.get("compared_samples") == want_n)
                    detail = (f", lag_frames={frames_k}: {d.get('status')} {d.get('compared_samples')} pairs "
                              f"from {d.get('from_sample')} (expected {want_n} from {want_from})")
            ok = ok and good
            out.append(f"  {'PASS' if good else 'FAIL'}: {label} summary frame {f} delay search: "
                       f"reference part [{ref_lo}, {ref_hi}), sdl held from {sdl0}, {pairs0} pairs at delay 0: "
                       f"{c.get('status')}, fixed_lag_min_samples {c.get('fixed_lag_min_samples')}, "
                       f"fixed_lag_min_frames {c.get('fixed_lag_min_frames')}{detail}")
        # Po ručním zastavení drží výstup o zpoždění méně než reference (na 1a6773d4 tu bylo
        # jen obecné no_overlap); po automatickém konci drží oba proudy od stejného indexu.
        good = hits >= 1 if not limit else True
        ok = ok and good
        out.append(f"  {'PASS' if good else 'FAIL'}: {label}: {hits} frame(s) at the memory edge without "
                   f"a start for the delay search{' (required after a manual stop)' if not limit else ''}")
        return ok
    except Exception as e:  # noqa: BLE001 - výsledek vlákna (i chybějící pole odpovědi)
        out.append(f"  FAIL: {label}: {e!r}")
        return False
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


#: Živé čtení: nejkratší doba a nejvýš doba čtení, dokud se nezastihnou všechny druhy oken.
_LIVE_MIN_S, _LIVE_MAX_S = 2.0, 20.0
#: Nejvýš pokusů o kontrolu obsahu nejnovějších snímků po zastavení (při 100 %).
_LIVE_STABLE_TRIES = 3


def _live_kind(a):
    """Druh okna podle živé odpovědi (jen u snímku s drženými hranicemi), jinak None."""
    fr = a["frame_range"]
    if fr["status"] != "complete":
        return None
    return fr.get("audio_status")


def run_live(exe, speed, out):
    """Případ 3 (speed 100 nebo 0 = MAX SPEED): dotazy během běžícího zachycení; vrátí True = prošlo.

    Na rychlosti stroje nezávisí, co se kontroluje, jen jak dlouho se čte:
    živé čtení (při zvolené rychlosti) běží aspoň _LIVE_MIN_S a dál, dokud
    nezastihne aspoň 40 odpovědí a okno complete, partial i lost (nejvýš
    _LIVE_MAX_S, pak selže s vysvětlením). Kontrola obsahu po zastavení
    potřebuje snímek, který paměť PCM (1 s) po zastavení ještě drží: při MAX
    SPEED projde emulace i za 50 ms zpoždění zastavení (zátěž stroje) víc než
    celou paměť (změřeno rev37_live_rozbor.py: asi 2000 snímků za sekundu,
    paměť 60 snímků), takže poslední čtení nejnovějších snímků proběhnou
    při 100 % (zachycení dál běží) a zastaví se hned po nich; když přesto
    žádný přečtený snímek nezůstane celý, zachycení se rozjede znovu
    (nejvýš _LIVE_STABLE_TRIES pokusů). Obsah se po zastavení porovná
    u všech živě přečtených celých snímků, které paměť ještě drží.

    Odpovědi a výřezy se počítají zvlášť: klíč (zdroj, snímek) se smí
    přečíst víckrát (mezi dvěma čteními nejnovějšího snímku se další
    neuzavřel). Projde jen tehdy, když po zastavení zůstal držený aspoň
    jeden nejnovější klíč, každý takový klíč se po zastavení ověřil se
    shodným obsahem a všechna živá čtení téhož celého výřezu mají stejný
    otisk. Na 1a0f1a18 test počítal shodné výřezy proti počtu odpovědí
    (2 >= 3) a při prodlevě klienta kolem návratu z MAX SPEED selhal
    i bez jediné chyby (posudek 1a0f1a18, prodleva.py).
    """
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmlive-"))
    emu = PipeEmu(exe, tmp)
    label = f"{exe.stem} live {'MAX SPEED' if speed == 0 else f'{speed} %'}"
    try:
        _setup(emu, speed)
        # souhrny snímků na 10 min: hranice všech čtených snímků jdou po zastavení ověřit i při MAX SPEED
        emu.call("audio_capture_start", dict(events=False, psg_writes=False, reference_wav=False, sdl_wav=False,
                                             memory_minutes=10, pcm=["reference", "sdl"], pcm_seconds=1))
        emu.call("run", {"frames": 80}, timeout=120.0)
        span = _pcm(emu, "reference", frame_from=70, frames=1)[0]["samples"]   # vzorků na snímek (PCM drží vše)
        emu.call("run", {})                                  # async: zachycení běží dál
        # complete_hash: otisky všech živých čtení celého snímku podle klíče (zdroj, snímek);
        # tentýž klíč se smí číst víckrát (živě i jako nejnovější), otisky se pak musí shodovat
        seen, complete_hash = [], {}
        closed = 80
        start = time.time()
        live_kinds = {}
        k = 0
        while True:
            now = time.time() - start
            covered = len(seen) >= 40 and all(live_kinds.get(x) for x in ("complete", "partial", "lost"))
            if (now >= _LIVE_MIN_S and covered) or now >= _LIVE_MAX_S:
                break
            source = _SOURCES[k % 2][0]
            # snímky u okraje přepisu: PCM 1 s = 44100 / (vzorků na snímek) snímků před posledním uzavřeným
            edge = 44100 // max(1, span)
            f = max(0, closed - edge - 2 + (k % 5)) if k % 4 else max(0, closed - 1)
            a, raw = _pcm(emu, source, frame_from=f, frames=1)
            closed = a["frame_range"]["closed_frames"]
            seen.append((source, f, a, hashlib.sha256(raw).hexdigest()))
            kind = _live_kind(a)
            live_kinds[kind] = live_kinds.get(kind, 0) + 1
            if kind == "complete":
                complete_hash.setdefault((source, f), []).append(hashlib.sha256(raw).hexdigest())
            k += 1
        live_s = time.time() - start
        if not covered:
            out.append(f"  FAIL: {label}: after {live_s:.1f} s of live reads {len(seen)} answers, "
                       f"live window kinds {live_kinds} (need complete, partial and lost)")
        # Nejnovější snímky při 100 % (zachycení běží dál), hned potom pauza: přečtený snímek
        # pak paměť PCM drží i při zpoždění zastavení do asi 1 s.
        if speed == 0:
            emu.call("set_speed", {"mode": "normal"})
        tries, stable, newest = 0, [], []
        while True:
            tries += 1
            newest = []
            for r in range(4):
                source = _SOURCES[r % 2][0]
                f = max(0, closed - 1)
                a, raw = _pcm(emu, source, frame_from=f, frames=1)
                closed = a["frame_range"]["closed_frames"]
                seen.append((source, f, a, hashlib.sha256(raw).hexdigest()))
                if _live_kind(a) == "complete":
                    key = (source, f)
                    complete_hash.setdefault(key, []).append(hashlib.sha256(raw).hexdigest())
                    newest.append(key)
            emu.call("pause")
            # odpovědi (newest) a různé výřezy (klíče) se počítají zvlášť: tentýž snímek
            # se mezi nejnovějšími může objevit dvakrát (snímek se mezi čteními neuzavřel)
            stable = [key for key in dict.fromkeys(newest)
                      if _live_kind(_pcm(emu, key[0], frame_from=key[1], frames=1)[0]) == "complete"]
            if stable or tries >= _LIVE_STABLE_TRIES:
                break
            emu.call("run", {})                              # žádný přečtený snímek nezůstal: znovu
        emu.call("audio_capture_stop")
        fields, rows, _ = read_rows(emu, "audio_frames", ["seq", "ref_first", "sdl_first"])
        bounds = {r[fields.index("seq")]: (r[fields.index("ref_first")], r[fields.index("sdl_first")]) for r in rows}
        # konec posledního uzavřeného snímku (další řádek není): index hranic po zastavení
        ends = []
        for s, _ in _SOURCES:
            a, _ = _pcm(emu, s, frame_from=len(rows) - 1, frames=1)
            ends.append(a["frame_range"].get("samples_requested", [0, a["next_sample"]])[1])
        bounds.setdefault(len(rows), tuple(ends))
        ok, kinds, bad = covered, {}, 0
        for source, f, a, _ in seen:
            fr = a["frame_range"]
            if fr["status"] != "complete":
                continue
            col = 0 if source == "reference" else 1
            if f not in bounds or f + 1 not in bounds:
                bad += 1
                out.append(f"  FAIL: {label} {source} frame {f}: no boundaries in audio_frames after stop")
                continue
            s0, s1 = bounds[f][col], bounds[f + 1][col]
            av0, av1 = a["available"]
            lo = min(s1, max(s0, av0))
            cnt = max(0, min(s1, av1) - lo)
            kind = "complete" if cnt == s1 - s0 else ("lost" if cnt == 0 else "partial")
            good = (fr.get("samples_requested") == [s0, s1] and fr.get("audio_status") == kind
                    and a["samples"] == cnt and (cnt == 0 or a["from_sample"] == lo)
                    and a["from_sample"] >= s0 and a["next_sample"] <= s1)
            kinds[kind] = kinds.get(kind, 0) + 1
            if not good:
                bad += 1
                if bad <= 5:
                    out.append(f"  FAIL: {label} {source} frame {f}: bounds {bounds.get(f)} / {bounds.get(f + 1)}, "
                               f"requested [{s0}, {s1}), available [{av0}, {av1}), "
                               f"got [{a['from_sample']}, {a['next_sample']})")
        # celý výřez přečtený živě víckrát: všechna živá čtení musí mít stejný obsah
        repeated = 0
        for (source, f), hs in complete_hash.items():
            if len(hs) > 1:
                repeated += 1
                if len(set(hs)) != 1:
                    bad += 1
                    out.append(f"  FAIL: {label} {source} frame {f}: {len(hs)} live reads of the complete frame "
                               f"differ ({len(set(hs))} distinct contents)")
        # obsah po zastavení: každý různý živě celý výřez, který paměť po zastavení drží celý
        same_keys = set()
        for (source, f), hs in complete_hash.items():
            a, raw = _pcm(emu, source, frame_from=f, frames=1)
            if _live_kind(a) == "complete":
                if hashlib.sha256(raw).hexdigest() != hs[0]:
                    bad += 1
                    out.append(f"  FAIL: {label} {source} frame {f}: content differs after stop")
                else:
                    same_keys.add((source, f))
        if not stable:
            out.append(f"  FAIL: {label}: no read newest frame still held after {tries} pause(s) at 100 %")
        # každý držený nejnovější výřez se musí po zastavení ověřit (shodný obsah)
        unchecked = [key for key in stable if key not in same_keys]
        if unchecked:
            out.append(f"  FAIL: {label}: newest frames held at pause not verified after stop: {unchecked}")
        ok = ok and bad == 0 and bool(stable) and not unchecked
        out.append(f"  {'PASS' if ok else 'FAIL'}: {label}: {len(seen)} reads while capturing ({live_s:.1f} s), "
                   f"audio status {dict(sorted(kinds.items()))}, {len(same_keys)} complete frames same after stop "
                   f"({len(newest)} newest reads, {len(stable)} distinct held after {tries} pause(s), "
                   f"{repeated} frames read live more than once), {bad} wrong")
        return ok
    except Exception as e:  # noqa: BLE001 - výsledek vlákna (i chybějící pole odpovědi)
        out.append(f"  FAIL: {label}: {e!r}")
        return False
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def run_all(exe, results):
    """Všechny případy jedné binárky; výsledek do `results[exe.stem]`."""
    out, ok = [], True
    for speed in (100, 50, 200):
        ok = run_frames(exe, speed, out) and ok
    for limit in (0, 171):
        ok = run_summary(exe, limit, out) and ok
    for speed in (100, 0):
        ok = run_live(exe, speed, out) and ok
    results[exe.stem] = (ok, out)


def main():
    """Spustí případy; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    pal = _find_exe("mz800emu")
    if pal is None:
        print("SKIP: no mz800emu binary found")
        return 77
    exes = [pal]
    ntsc = _find_exe("mz700emu-ntsc")
    if ntsc is not None:
        exes.append(ntsc)
    results = {}
    threads = [threading.Thread(target=run_all, args=(e, results)) for e in exes]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ok = len(results) == len(exes)
    for e in exes:
        good, lines = results.get(e.stem, (False, [f"  FAIL: {e.stem}: no result"]))
        print(f"\n=== {e.stem} ===")
        print("\n".join(lines))
        ok = ok and good
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
