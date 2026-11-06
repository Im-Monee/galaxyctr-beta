"""Compares every J3D material's Wii TEV (reference, Dolphin's rules) with the PICA combiners
mat.py compiles for it, on random inputs, with the file's TEV colours and with each key of the
model's colour animations (.brk). Usage: tevcheck.py <tools/3ds/native> <game files> [filter]"""
import sys, os, struct, random, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_native, n3d, mat as M, j3d
from build_native import yaz0

random.seed(1)

# ---- GX reference --------------------------------------------------------------------------

# ##############################################################################################
# # The Wii's TEV, evaluated                                                                   #
# #                                                                                            #
# # gx_eval computes what a material's TEV stages give for given inputs, by Dolphin's rules    #
# # (signed 11-bit registers, the bias, scale and clamp of each stage).                        #
# ##############################################################################################
def s11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v

def gx_eval(mat, tev_colors, tex, ras):
    """tex: {(map, coord): (r,g,b,a)}, ras: [chan0 rgba, chan1 rgba]. Returns (r,g,b,a) 0..255."""
    regs = [list(tev_colors[3])] + [list(c) for c in tev_colors[:3]]  # PREV, REG0, REG1, REG2
    regs = [[s11(x) for x in r] for r in regs]
    for s in range(mat["stage_count"]):
        coord, tmap, chan = mat["orders"][s]
        ras_sel, tex_sel = mat["swap_modes"][s]
        tsw = mat["swap_tables"][tex_sel]
        rsw = mat["swap_tables"][ras_sel]
        if tmap != 0xFF:
            t0 = tex[(tmap, coord)]
            t = [t0[tsw[0]], t0[tsw[1]], t0[tsw[2]], t0[tsw[3]]]
        else:
            t = [255, 255, 255, 255]
        rc = {4: 0, 5: 1, 0: 0, 1: 1, 2: 0, 3: 1}.get(chan)
        if rc is None:
            r = [0, 0, 0, 0]
        else:
            r0 = ras[rc]
            r = [r0[rsw[0]], r0[rsw[1]], r0[rsw[2]], r0[rsw[3]]]
        kc = mat["kcsel"][s]
        ka = mat["kasel"][s]
        kcol = mat["konst_colors"]

        def konst_c():
            if kc < 8:
                v = M.KONST_FRACTIONS[kc]
                return [v, v, v]
            k = kcol[kc & 3]
            if 0x0C <= kc <= 0x0F:
                return list(k[:3])
            if kc >= 0x10:
                c = (kc - 0x10) >> 2
                return [k[c]] * 3
            return [255] * 3

        def konst_a():
            if ka < 8:
                return M.KONST_FRACTIONS[ka]
            k = kcol[ka & 3]
            if ka >= 0x10:
                return k[(ka - 0x10) >> 2]
            return 255

        def cin(arg):
            if arg <= 7:
                reg = regs[arg // 2]
                return reg[:3] if arg % 2 == 0 else [reg[3]] * 3
            if arg == 14:
                return konst_c()
            return {8: t[:3], 9: [t[3]] * 3, 10: r[:3], 11: [r[3]] * 3, 12: [255] * 3, 13: [128] * 3, 15: [0] * 3}[arg]

        def ain(arg):
            if arg <= 3:
                return regs[arg][3]
            if arg == 6:
                return konst_a()
            return {4: t[3], 5: r[3], 7: 0}[arg]

        (ca, cb, cc, cd, cop, cbias, cscale, cclamp, creg), (aa, ab, ac, ad, aop, abias, ascale, aclamp, areg) = mat["stages"][s]
        if (cop >= 2 and cop != 8) or aop >= 2:
            return None  # compare modes: only GX_TEV_COMP_R8_GT (colour) is modelled

        def lerp(a, b, c, d, op, bias, scale, clamp):
            a &= 255; b &= 255; c &= 255
            c2 = c + (c >> 7)
            v = (a * (256 - c2) + b * c2) / 256.0
            x = d - v if op == 1 else d + v
            x += {0: 0, 1: 128, 2: -128}.get(bias, 0)
            x *= {0: 1, 1: 2, 2: 4, 3: 0.5}[scale]
            x = int(round(x))
            if clamp:
                return min(255, max(0, x))
            return s11(x)

        A, B, C, D = cin(ca), cin(cb), cin(cc), cin(cd)
        if cop == 8:  # d + (a.r > b.r ? c : 0), 8 bits
            hit = (A[0] & 255) > (B[0] & 255)
            out_c = [D[i] + ((C[i] & 255) if hit else 0) for i in range(3)]
            out_c = [min(255, max(0, x)) if cclamp else s11(x) for x in out_c]
        else:
            out_c = [lerp(A[i], B[i], C[i], D[i], cop, cbias, cscale, cclamp) for i in range(3)]
        out_a = lerp(ain(aa), ain(ab), ain(ac), ain(ad), aop, abias, ascale, aclamp)
        regs[creg & 3][:3] = out_c
        regs[areg & 3][3] = out_a
    p = regs[0]
    return [min(255, max(0, x)) for x in p]


# ---- PICA ----------------------------------------------------------------------------------

def ref_value(ref, tev_colors, konst, fallback):
    kind, index, part, negate = ref
    if kind == M.REF_TEVREG and 1 <= index <= 3:
        c = tev_colors[index - 1]
    elif kind == M.REF_KONST and index <= 3:
        c = konst[index]
    else:
        return fallback
    sign = -1 if negate == 1 else 1
    f = {2: lambda v: v & 0xFF, 3: lambda v: min(255, max(0, v + 128)), 4: lambda v: min(255, max(0, v - 255)),
         5: lambda v: min(255, max(0, (v - 255) >> 1)), 6: lambda v: min(255, max(0, -v >> 1))}.get(negate, lambda v: min(255, max(0, sign * v)))
    if part == M.PART_RGB:
        return [f(c[i]) for i in range(3)]
    v = f(c[3 if part == M.PART_ALPHA else part - M.PART_R])
    return [v, v, v]

# ##############################################################################################
# # The compiled combiners, evaluated                                                          #
# #                                                                                            #
# # pica_eval computes what the PICA's combiner stages compiled by mat.py give for the same    #
# # inputs.                                                                                    #
# ##############################################################################################
def pica_stages(stages, units, ras_channel, tev_colors, konst, tex, ras, buffer_masks=(0, 0)):
    prim = [x / 255.0 for x in ras[ras_channel]]
    prev = [0.0, 0.0, 0.0, 0.0]
    buf = [128 / 255.0] * 4
    for index, st in enumerate(stages):
        const = list(st.constant)
        if st.color_ref is not None:
            const[0:3] = ref_value(st.color_ref[1], tev_colors, konst, const[0:3])
        if st.alpha_ref is not None:
            const[3] = ref_value(st.alpha_ref[1], tev_colors, konst, [const[3]] * 3)[0]
        const = [x / 255.0 for x in const]

        def src(code):
            if code in (0, 1):
                return prim
            if 3 <= code <= 5:
                u = code - 3
                if u >= len(units):
                    return [0.0] * 4
                return [x / 255.0 for x in tex[(units[u][0], units[u][1])]]
            if code == 0xE:
                return const
            if code == 0xD:
                return list(buf)
            return prev

        def cop(v, op):
            base = {0: v[:3], 2: [v[3]] * 3, 4: [v[0]] * 3, 8: [v[1]] * 3, 0xC: [v[2]] * 3}[op & ~1]
            return [1 - x for x in base] if op & 1 else base

        def aop(v, op):
            x = v[{0: 3, 2: 0, 4: 1, 6: 2}[op & ~1]]
            return 1 - x if op & 1 else x

        def fn(func, a, b, c):
            return {0: a, 1: a * b, 2: a + b, 3: a + b - 0.5, 4: a * c + b * (1 - c), 5: a - b,
                    8: a * b + c, 9: min(1.0, a + b) * c}[func]

        cs = [cop(src(st.color_src[i]), st.color_op[i]) for i in range(3)]
        al = [aop(src(st.alpha_src[i]), st.alpha_op[i]) for i in range(3)]
        sc = [1, 2, 4][st.color_scale]
        sa = [1, 2, 4][st.alpha_scale]
        oc = [min(1.0, max(0.0, fn(st.color_func, cs[0][i], cs[1][i], cs[2][i]) * sc)) for i in range(3)]
        oa = min(1.0, max(0.0, fn(st.alpha_func, al[0], al[1], al[2]) * sa))
        prev = oc + [oa]
        if index < 4 and buffer_masks[0] & (1 << index):
            buf[0:3] = oc
        if index < 4 and buffer_masks[1] & (1 << index):
            buf[3] = oa
    return prev

def pica_eval(compiled, tev_colors, konst, tex, ras):
    stages, units, ras_channel, inexact, passes = compiled[:5]
    fb = pica_stages(stages, units, ras_channel, tev_colors, konst, tex, ras, compiled[5] if len(compiled) > 5 else (0, 0))
    for pst, blend, channel, pass_units in passes:
        src = pica_stages(pst, pass_units, channel, tev_colors, konst, tex, ras)
        eq, sf, df = blend
        def factor(f, i):
            return {M.FACTOR_ZERO: 0.0, M.FACTOR_ONE: 1.0, M.FACTOR_SRC_COLOR: src[i], M.FACTOR_DST_COLOR: fb[i]}[f]
        out = []
        for i in range(3):
            s, d = src[i] * factor(sf, i), fb[i] * factor(df, i)
            v = s + d if eq == M.BLEND_ADD else (s - d if eq == M.BLEND_SUBTRACT else d - s)
            out.append(min(1.0, max(0.0, v)))
        fb = out + [fb[3]]
    return [int(round(x * 255)) for x in fb]


# ---- colour animations -----------------------------------------------------------------------

# ##############################################################################################
# # Checking every material                                                                    #
# #                                                                                            #
# # Both are compared on random inputs and on the colour register values the archive's         #
# # animations set; a material whose results differ is reported. far.py also uses gx_eval for  #
# # the far models' colours.                                                                   #
# ##############################################################################################
def brk_keys(members):
    """{material name: {register (1-3): [(r, g, b, a) per key]}} from the .brk files."""
    out = {}
    for name, c in members:
        if not name.lower().endswith(".brk") or len(c) < 0x60:
            continue
        if c[:8] == b"J3D1brk1":
            bo = ">"
        elif c[:8] == b"1D3J1krb":
            bo = "<"
        else:
            continue
        p = 0x20
        creg_count = struct.unpack_from(bo + "H", c, p + 0x0C)[0]
        offs = struct.unpack_from(bo + "10I", c, p + 0x20)
        tab, ids, nametab = p + offs[0], p + offs[2], p + offs[4]
        names = M.name_table(c, nametab, bo) if offs[4] else []
        chans = [p + offs[6 + k] for k in range(4)]
        for i in range(creg_count):
            e = tab + i * 0x1C
            seq = []
            for k in range(4):
                cnt, off, tan = struct.unpack_from(bo + "3H", c, e + k * 6)
                if cnt == 1:
                    seq.append([struct.unpack_from(bo + "h", c, chans[k] + off * 2)[0]])
                else:
                    stride = 4 if tan else 3
                    seq.append([struct.unpack_from(bo + "h", c, chans[k] + (off + j * stride + 1) * 2)[0] for j in range(cnt)])
            color_id = c[e + 0x18]
            n = max(len(s) for s in seq)
            keys = [tuple(s[min(j, len(s) - 1)] for s in seq) for j in range(n)]
            nm = names[i] if i < len(names) else None
            if nm is not None and color_id < 3:
                out.setdefault(nm, {}).setdefault(color_id + 1, []).extend(keys)
    return out


# ---- driver --------------------------------------------------------------------------------

def rnd_color():
    pick = random.random()
    if pick < 0.15:
        return tuple(random.choice((0, 255)) for _ in range(4))
    return tuple(random.randint(0, 255) for _ in range(4))

def check_material(m, compiled, regsets, samples=48):
    worst = 0
    worst_case = None
    maps = set()
    for s in range(m["stage_count"]):
        coord, tmap, chan = m["orders"][s]
        if tmap != 0xFF:
            maps.add((tmap, coord))
    for tev in regsets:
        for _ in range(samples):
            tex = {k: rnd_color() for k in maps}
            ras = [rnd_color(), rnd_color()]
            g = gx_eval(m, tev, tex, ras)
            if g is None:
                return None
            p = pica_eval(compiled, tev, m["konst_colors"], tex, ras)
            if p is not None and m["blend"][:3] == (1, 1, 1) and m["alpha_comp"][:4] == (4, 0, 0, 7):
                # Additive (ONE, ONE) with the alpha test of compare_stage: a discarded pixel adds
                # nothing, and the alpha only gates (the blend does not read it).
                p = ([0, 0, 0] if p[3] <= 0 else list(p[:3])) + [g[3]]
            # Alpha matters only with blending or an alpha test; colour always.
            blend_alpha = m["blend"][0] == 1 or M.fragment_state(m)[3] != M.TEST_ALWAYS
            err = max(abs(g[i] - p[i]) for i in range(3))
            if blend_alpha:
                err = max(err, abs(g[3] - p[3]))
            if err > worst:
                worst, worst_case = err, (tev, tex, ras, g, p)
    return worst, worst_case

def iter_archives(root):
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.lower().endswith(".arc"):
                yield os.path.join(dp, f)

def main():
    root = sys.argv[2]
    filt = sys.argv[3].lower() if len(sys.argv) > 3 else None
    results = []
    global allclamped
    allclamped = []
    counts = collections.Counter()
    for path in iter_archives(root):
        if filt and filt.startswith("@"):
            if os.path.splitext(os.path.basename(path))[0] not in open(filt[1:]).read().split():
                continue
        elif filt and filt not in path.lower():
            continue
        data = open(path, "rb").read()
        if yaz0.is_compressed(data):
            data = yaz0.decompress(data)
        try:
            members = list(build_native.rarc_files(data))
        except Exception:
            continue
        animated = n3d.animated_registers(members)
        keys = brk_keys(members)
        for name, c in members:
            if not n3d.is_j3d_model(c):
                continue
            if build_native.is_converted_j3d(c):
                c = j3d.restore_model(c)
            b = n3d.blocks(c)
            if "MAT3" not in b:
                continue
            for mi, m in enumerate(M.parse(c, *b["MAT3"])):
                m["animated_registers"] = animated.get(m["name"], {})
                compiled = M.compile_material(m)
                regsets = [m["tev_colors"]]
                for reg, ks in keys.get(m["name"], {}).items():
                    for k in ks:
                        tc = [list(x) for x in m["tev_colors"]]
                        tc[reg - 1] = list(k)
                        regsets.append(tc)
                r = check_material(m, compiled, regsets)
                rs = check_material(m, compiled, regsets[:1]) if r is not None else None
                if r is not None and r[0] > 6 and not compiled[3]:
                    counts["wrong with file colours" if rs[0] > 6 else "wrong only with animation"] += 1
                    counts["wrong, with passes" if compiled[4] else "wrong, single pass"] += 1
                    unclamped = any(m["stages"][s][0][7] == 0 or m["stages"][s][1][7] == 0 for s in range(m["stage_count"] - 1))
                    counts["wrong, unclamped stage before the last" if unclamped else "wrong, all clamped"] += 1
                    if not unclamped:
                        allclamped.append((r[0], os.path.relpath(path, root), name, mi, m["name"]))
                counts["materials"] += 1
                if r is None:
                    counts["compare mode (skipped)"] += 1
                    continue
                worst, case = r
                inexact = compiled[3]
                counts["inexact" if inexact else "exact"] += 1
                if worst > 6:
                    counts["wrong, marked inexact" if inexact else "WRONG, marked exact"] += 1
                results.append((worst, inexact, os.path.relpath(path, root), name, mi, m["name"], case))
    if os.environ.get("TEVDUMP"):
        with open(os.environ["TEVDUMP"], "w", encoding="utf-8") as f:
            for worst, inexact, path, name, mi, mname, case in results:
                f.write("%s\t%s\t%d\t%s\t%d\t%d\n" % (path, name, mi, mname, worst, inexact))
    print(dict(counts))
    for x in sorted(allclamped, reverse=True)[:60]:
        print("CLAMPED", x)
    for x in results:
        if x[1]:
            print("INEXACT", x[0], x[2], x[3], x[4], x[5])
    results.sort(key=lambda x: (x[1], -x[0]))
    shown = 0
    for worst, inexact, path, name, mi, mname, case in results:
        if worst <= 6 or inexact:
            continue
        tev, tex, ras, g, p = case
        print("%3d %s %s mat %d %s  gx %s pica %s  tev %s tex %s ras %s" % (worst, path, name, mi, mname, g, p, tev, tex, ras))
        shown += 1
        if shown >= 2000:
            break

if __name__ == "__main__":
    main()
