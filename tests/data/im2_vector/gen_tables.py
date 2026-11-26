#!/usr/bin/env python3
"""Vygeneruje tabulky pro im2_vector.asm: tabulku IM 2 (257 bajtů) a inverzní tabulku.

Tabulka IM 2 je de Bruijnova posloupnost B(16, 2) nad bajty 60h až 6Fh, prodloužená o první
prvek. Záznam pro vektor v je slovo b[v] + 256 * b[v + 1]; protože se každá dvojice
sousedních prvků v posloupnosti vyskytuje jen jednou, vede každý z 256 vektorů na jinou
adresu v oblasti 6060h až 6F6Fh (dolní bajt i stránka 60h až 6Fh).

Inverzní tabulka (256 bajtů) převádí klíč h * 16 + j (h = index stránky b[v + 1] - 60h,
j = index dolního bajtu b[v] - 60h) zpět na vektor v.

Použití: gen_tables.py <výstup.inc>
"""

import sys

K = 16          #: velikost abecedy
BASE = 0x60     #: první bajt abecedy (stránky i dolní bajty cílových adres)


def de_bruijn(k, n):
    """Vrátí cyklickou de Bruijnovu posloupnost B(k, n) jako seznam indexů 0 až k - 1.

    Standardní algoritmus přes Lyndonova slova; délka výsledku je k ** n.
    """
    a = [0] * k * n
    seq = []

    def db(t, p):
        if t > n:
            if n % p == 0:
                seq.extend(a[1:p + 1])
        else:
            a[t] = a[t - p]
            db(t + 1, p)
            for j in range(a[t - p] + 1, k):
                a[t] = j
                db(t + 1, t)

    db(1, 1)
    return seq


def main():
    seq = de_bruijn(K, 2)
    assert len(seq) == K * K
    seq = seq + [seq[0]]                     # 257 prvků, všech 256 dvojic lineárně
    inv = [None] * 256
    for v in range(256):
        key = (seq[v + 1] << 4) | seq[v]
        assert inv[key] is None, "dvojice se opakuje"
        inv[key] = v
    assert None not in inv
    with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
        f.write("; im2_tables.inc - vygenerováno gen_tables.py, needitovat\n")
        f.write("; im2tab: 257 bajtů tabulky IM 2 (de Bruijn B(16, 2) nad 60h až 6Fh)\n")
        f.write("; invtab: klíč h * 16 + j -> vektor (h = stránka - 60h, j = dolní bajt - 60h)\n")
        f.write("im2tab:\n")
        b = [BASE + x for x in seq]
        for i in range(0, 257, 16):
            f.write("        db      " + ", ".join(f"0{x:02X}h" for x in b[i:i + 16]) + "\n")
        f.write("invtab:\n")
        for i in range(0, 256, 16):
            f.write("        db      " + ", ".join(f"0{x:02X}h" for x in inv[i:i + 16]) + "\n")
    print(f"wrote {sys.argv[1]}: 257 + 256 bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
