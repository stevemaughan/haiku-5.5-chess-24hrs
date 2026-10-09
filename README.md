# Haiku 5.5 chess 24hrs: a from-scratch UCI engine built in 24 hours

**Official rating:** **2765 Elo ±24** (95% CI, CCRL Blitz scale), seventh of eight AI engines in the series;
see [Official results](#official-results).

Written for a reader who knows chess programming but nothing about this benchmark.

## What this is

A benchmark in which one AI model (Haiku 5.5, `claude-haiku-5-5`) had 24 hours of wall-clock time to design, build, test and deliver a UCI chess engine, starting from an empty folder. Only the playing strength of the final executable counts. The engine is rated at **10 seconds + 0.1 second increment**, single-threaded, against other engines (including Stash 20 to 37) using the UHO opening book, in fastchess.

The rules, from the benchmark's `CLAUDE.md`:

- Written from scratch in the session. Published documentation and tables may be used (chessprogramming.org is allowed); copying engine source code, neural networks, opening books or endgame data is not.
- Standard library and compiler intrinsics only. GCC (MinGW-w64), C or C++.
- `final/` at the deadline is the entry. It must play legal chess, speak UCI, manage its clock, be statically linked, and be compiled for `x86-64-v3` (never `-march=native`).
- Keep `docs/progress.md` (hourly log), `docs/resources.md` (internet resources consulted) and snapshots of every verified build.
- Do not modify `resources/`.

## The engine

- **Name:** `Haiku 5.5 chess 24hrs` (`id name`). **Author:** `Haiku 5.5`. **Executable:** `final/Haiku55chess24hrs.exe`.
- **Language:** C++17, compiled with GCC 15.2.0 (MinGW-w64, posix threads). C++ because the toolchain was provided, the code needs no dependencies, and PEXT is available through `immintrin.h`.
- **Build** (exact command, from `source/build_final.sh`):

  ```
  g++ -std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -mtune=generic -flto -static -s \
      -o final/Haiku55chess24hrs.exe source/haiku.cpp -lpthread
  ```

  Development builds used `-O2` or `-O3` without `-s`. The release imports only `KERNEL32.dll` and `msvcrt.dll`.
- **Run:** start `Haiku55chess24hrs.exe` and speak UCI on stdin/stdout. Two extra debugging entry points exist: `Haiku55chess24hrs.exe --perft-suite <epd> <maxdepth>` and the UCI command `perft N`.
- **UCI options:** `Hash` (spin, default 256 MB, range 1 to 2048) and `Move Overhead` (spin, default 30 ms, range 0 to 5000). No others.

## Architecture and features

- **Board:** 64-bit bitboards for each piece and colour, plus a mailbox array for piece-on-square lookups. Make/unmake with a per-ply undo record. Zobrist hashing, with the key history kept for repetition detection.
- **Move generation:** pseudo-legal generation, then legality by make/unmake and an attack test on the king. Sliding attacks come from PEXT-indexed tables built at start-up (102,400 rook and 5,248 bishop entries). Castling checks the squares the king passes over.
- **Correctness:** the perft suite (`resources/perft/perft.epd`) passes all 126 positions to depth 6. The start-position perft 6 is 119,060,324, which matches the published value.
- **Search:**
  - iterative deepening with aspiration windows from depth 5;
  - principal variation search with a triangular PV table;
  - a transposition table of 16-byte entries (256 MB by default), with age-based replacement, and `hashfull` reported in `info` lines;
  - null-move pruning (reduction 3 + depth/4 + a margin from the static evaluation; not in check, not in pawn-only endings);
  - reverse futility pruning at depth up to 6;
  - futility pruning of quiet moves at depth up to 4;
  - late-move pruning, with the "improving" flag widening the allowance;
  - late-move reductions from a logarithmic table, adjusted for improving, history and checking moves;
  - internal iterative reduction (depth minus one when a node has no transposition-table move);
  - a check extension;
  - killer moves and a gravity-decayed history heuristic;
  - mate-distance pruning, and draws by repetition and the 50-move rule.
- **Quiescence:** stand pat, MVV-LVA ordering, captures filtered by static exchange evaluation (SEE), delta pruning, and all legal evasions when in check.
- **Evaluation:**
  - material and piece-square tables from the Simplified Evaluation Function (values from chessprogramming.org, not tuned by me), interpolated between middlegame and endgame by remaining material;
  - bishop pair bonus;
  - isolated and doubled pawn penalties, and passed-pawn bonuses by rank;
  - rooks on semi-open and open files;
  - mobility for knights, bishops, rooks and queens (squares not occupied by own pieces and not covered by enemy pawns);
  - king safety (enemy attacks on the king's surrounding squares, with a penalty when two or more attackers are present, and a pawn-shield bonus for a king on its back rank);
  - a small tempo bonus.
- **Time management:** soft and hard limits derived from the remaining time and the increment. With time to spare, the soft limit is (remaining − overhead)/30 + 0.75 × increment. Under 2 s it is smaller, and the hard limit is tighter. A new iteration is not started if its predicted time (2.2 × the last iteration) would take the search past the soft limit (allowed to run 25% past it when the clock is comfortable). The hard limit is checked every 1024 nodes. `movestogo`, `movetime`, `depth` and `infinite` are honoured.
- **Threads:** one search thread plus an input thread, so `stop`, `isready` and `quit` are handled during a search.

Deliberately left out: singular extensions (tested, −31 Elo, see below), ProbCut, razoring, null-move verification, countermove heuristic, SEE pruning of losing captures, quiescence transposition table, pawn-hash cache (speed only), neural networks (not permitted), opening book (the harness supplies openings), tablebases, multi-threaded search, pondering and MultiPV.

## How the time was spent

Timeline in local time (America/New_York, UTC−4 in October), 2026-10-07 to 2026-10-08:

| Time | What happened |
|---|---|
| 20:05 | Run started. Records written (`docs/start_time.txt`, `progress.md`, `resources.md`). |
| 20:13 | First engine written in one file (about 1,000 lines at this point; 1,854 lines at the end). Perft to depth 5 passes. `--compliance` passes. |
| 20:14 | First legal game: 20 games against Stash 20. |
| 20:16–20:40 | Evaluation v2 (bishop pair, pawn structure, passed pawns, open files): +106 ± 29 in 400 self-play games. Promoted. |
| 20:41–21:06 | Mobility term: SPRT +49 ± 25 (438 games). Promoted. |
| 21:13–21:58 | Time forfeits found in the test PGNs. Investigated from fastchess's engine trace: the clock drained over about 15 moves (not a hang). Time management fixed. |
| 22:03–23:04 | First 200-game Stash 21 anchor (−47 ± 45). King-safety term: SPRT +22 ± 16 (1,118 games). Promoted. |
| 23:05–23:40 | Improving flag and late-move pruning: SPRT +29 ± 18 (710 games). Promoted. |
| 23:50–03:40 | 300-game Stash 21 anchor (+23 ± 33). Singular extension (rejected, −31). Time-budget variants (rejected). Extra positional terms and SEE pruning (rejected). Single-pass evaluation (speed, +10–12%, identical search). History-adjusted LMR: SPRT +23 ± 13 (1,302 games). Promoted. |
| 03:40–03:55 | 300-game Stash 25 anchor on the release (−215 ± 41). |
| 04:00–05:40 | Quiescence TT, null-move reduction 4, passed-pawn ×1.5 (none adopted). |
| 05:58 | 200-game Stash 21 anchor (+44 ± 40). |
| 06:00–08:00 | Countermoves, king-penalty ×2, futility margins 150 and 80 (none adopted). |
| 08:00–08:05 | Robustness pass: a FEN with a missing king crashed the engine. Fixed. |
| 08:20–09:07 | Internal iterative reduction: SPRT +22 ± 13 (1,306 games). Promoted. |
| 09:30–11:00 | Razoring, LMR divisor, improving reverse futility, ProbCut (all rejected). |
| 11:06 | Perft to depth 6 passes (126/126). |
| 11:16 | 200-game Stash 21 anchor on the release: +51 ± 46. |
| 11:25–14:05 | Null-move verification, history bonus cap, aspiration width, mobility ×1.6, tempo ×2 (none adopted). Clean-up of leftover switches. |
| 14:10–16:45 | Five parameter SPRTs back to back (A–E), all flat or negative, none adopted. |
| 16:45–17:05 | Final rebuild and verification, Stash 25 re-anchor on the final build. |
| 17:05 | Engine work ended with about three hours left. The last batch of tests was flat, and no further change was planned. This README was written then, under the protocol's allowance for an early write-up once the final build is installed and no more changes are planned. |

What was **measured**: every adopted change has an SPRT or a fixed-size self-play result against the previous release. Strength was anchored against Stash 20, 21 and 25 with fastchess at 10+0.1.

What was **accepted on judgement**: the evaluation's tapered tables and piece values (taken from the Simplified Evaluation Function, not tuned); the king-safety and mobility weights (set by hand and then tested as a whole); the time budget's constants (30 moves left, the increment shares, the 2.2× prediction factor), of which 30 was tested against 24 and 40 and kept; the FEN and robustness fixes, which do not change the search and were checked by node count.

What went **wrong**:

- Time forfeits in early test runs. The first time-management formula let the clock drain over about 15 moves at 10+0.1. The fix cost about 10 Elo (within noise) and removed the forfeits.
- A crash on a malformed FEN (a missing or duplicated king), found only in a deliberate robustness pass. It could have cost a game in the rating match.
- Many SPRTs were stopped early because the result was flat, which saves time but leaves those results less precise. These are marked in `progress.md`.
- Singular extensions were rejected at −31 Elo. I did not debug the implementation, so this says "not as implemented".
- Hourly log entries are missing for some hours (around 03:00–05:00, 06:00–08:00 and 12:00–14:00). The table below marks these.
- Early anchor results were from 20 and 40 game matches, which are too noisy to use alone.

## Elo by hour

Estimates are CCRL-equivalent (Stash's CCRL 2'+1" blitz scale). They come from match results against Stash 21 and Stash 25, not from a rating list. "Carried" means no new estimate was logged that hour, so the most recent one is shown.

| Time | Elo estimate | Basis |
|---|---|---|
| 20:05 (hour 0) | not yet measurable | Start of run. |
| 21:05 (hour 1) | about 2550–2700 | Stash 20 baseline (20 games, +17 ± 168) plus the first self-play gain (eval v2, +106). |
| 22:05 (hour 2) | about 2600–2700 | Stash 21 anchor (−47 ± 45) combined with the Stash 20 result. |
| 23:05 (hour 3) | about 2650–2750 | Self-play gains logged so far (eval v2 +106, mobility +49). |
| 00:05 (hour 4) | carried: 2650–2750 | No new estimate logged. |
| 01:05 (hour 5) | about 2700–2750 | Stash 21 anchor, 300 games (+23 ± 33). |
| 02:05 (hour 6) | about 2700–2750 | Carried. |
| 03:05 (hour 7) | carried: 2700–2750 | History-LMR accepted (+23 ± 13) after this log point. No new anchor. |
| 04:05 (hour 8) | carried | No entry logged. |
| 05:05 (hour 9) | about 2725–2735 | Stash 25 anchor, 300 games (−215 ± 41). |
| 06:05 (hour 10) | carried | Stash 21 anchor (+44 ± 40) at 05:58 implies about 2750. Not logged. |
| 07:05 (hour 11) | carried | No entry logged. |
| 08:05 | carried | Robustness pass. No estimate. |
| 09:05 | carried | Internal iterative reduction (+22 ± 13) after this log point. No new anchor. |
| 10:05 | carried | No entry logged. |
| 11:05 | about 2760 ± 50 | Stash 21 anchor on the release, 200 games (+51 ± 46). |
| 12:05–16:05 | carried: about 2760 ± 50 | No new anchor. Later changes were rejected. |
| 17:05 | about 2755 ± 50 | Stash 25 anchor on the final build, 300 games (−188 ± 39). |
| 18:05–19:05 | carried: about 2755 ± 50 | No new anchor. |
| 20:05 (end) | about 2755 ± 50 | Final estimate. |

## Assumptions

- **Rules:** the rules in `CLAUDE.md` as written. The model interprets "from scratch" as no code or data taken from other engines. Published piece-square values are allowed.
- **Hardware:** this machine (Intel i7-1260P, 12 physical cores) was used for all tests; the rating machine is assumed comparable. The target CPU is assumed to have BMI2 with fast PEXT (not Zen 1/2). The release does not use AVX2 or AVX-512 instructions explicitly, only the `x86-64-v3` baseline.
- **Harness:** fastchess 1.8.2 with the UHO book, `-repeat`, `plies=16`, and `-concurrency 10` (12 cores minus 2). The `timemargin` used by the rating harness is unknown; the engine subtracts a 30 ms `Move Overhead` from its budget.
- **Opponents:** Stash 20, 21 and 25 are the anchors. Their CCRL ratings are at a different time control from 10+0.1, so the mapping is approximate.
- **Hash:** the rating harness sets `Hash` to 256 MB. The engine's own default is also 256 MB.
- **Stash time losses:** in the anchor runs, Stash lost a few games on time (about 3 in total) while 20 engine processes shared the CPU. This may slightly inflate our score in those anchors.

## Estimated strength

**About 2755 CCRL-equivalent, ±50.** Evidence:

- Stash 21, 200 games at 10+0.1 on the final release: 97 W / 68 L / 35 D (57.25%), **+51 ± 46**. Stash 21 is about 2710 CCRL, so this gives about 2761.
- Stash 25, 300 games at 10+0.1 on the final release (rerun at 16:55): 46 W / 194 L / 60 D (25.33%), **−188 ± 39**. Stash 25 is about 2940 CCRL, so this gives about 2752. The two anchors agree within their error bars.
- Stash 21, 300 games on an earlier release (before history-LMR and IIR): +23 ± 33. Stash 21 + 23 = about 2733.
- Stash 25, 300 games on the release before IIR (03:40): 43 W / 208 L / 49 D (22.5%), **−215 ± 41**. Stash 25 minus 215 = about 2725. Superseded by the final-build run above.
- Stash 20, a 40-game sanity match on the final release: 30 W / 3 L / 7 D (83.75%). This is consistent with a large lead over Stash 20, but 40 games is too few for a precise estimate.
- Self-play (same time control, previous release as the opponent): eval v2 +106 ± 29, mobility +49 ± 25, king safety +22 ± 16, improving and late-move pruning +29 ± 18, history-adjusted LMR +23 ± 13, internal iterative reduction +22 ± 13. These gains are not additive against Stash, and only the Stash anchors place the engine on the CCRL scale.

Uncertainty: the ±50 covers anchor sampling error and the mapping from fastchess at 10+0.1 to CCRL blitz. Both anchors (11:16 and 17:02) were played on the final search and evaluation; the later changes were a cleanup of switches, which I checked by node count, and experiments that were all rejected. Stash 25 is the stronger anchor, so the estimate rests mostly on the two results agreeing. I would put the true rating somewhere between 2700 and 2800 on the CCRL scale.

Total games played in test runs (all tests, including the stopped and rejected ones): about 24,000, recorded in `source/tests/*.pgn`.

## Official results

Rated by the benchmark operator on 8 October 2026 in 1,060 games, added to the series'
existing rating dataset (10,380 games in total across all eight engines).

**Haiku 5.5 chess 24hrs: 2765 Elo ±24 (95% CI), CCRL Blitz scale**, from 1,060 games
(+388 =214 −458, 46.7% score). That is seventh of eight in the series, about 60 Elo above
Sonnet 5 and about 280 Elo below Fable 5.

The in-session estimate of about 2755 ±50 was close. Measured only against the five
anchor engines, Haiku 5.5 rates **2768 ±27**, and each anchor on its own implies
2734–2820. The games against the other AI engines agree: 58.1% against Sonnet 5 implies
about 2760, and 15.0% against Fable 5 implies about 2744.

### How the rating was measured

- Tool: fastchess 1.8.2, time control **10 s + 0.1 s**, one thread, **64 MB hash**,
  `timemargin 200`. (The development runs used Hash 256 and 16-ply openings.)
- Openings: UHO unbalanced 8-ply book, random order, every opening played with
  both colours.
- Adjudication: resign after 3 moves at ±600 cp (two-sided); draw after move 40
  when 8 consecutive moves stay within ±20 cp; 250-move cap. `-recover` on.
- Format: a 72-game pilot (41.7% against Stash 21, Juggernaut and Stash 25) placed it
  near 2740. Haiku 5.5 then played a gauntlet of 160 games each against five anchors
  (Stash 20, Stash 21, Juggernaut, Stash 25 and Crafty, the same set used for Sonnet 5),
  plus 160 games against Sonnet 5 and 100 against Fable 5. The stronger AI engines were
  left out because they are ~380+ Elo stronger. Ratings come from an anchored
  maximum-likelihood Elo fit over all games, with the anchors fixed at their CCRL Blitz
  ratings.
- Hardware: Ryzen AI 9 HX 375 (12 cores), 12 games at a time.
- Clean run: Haiku 5.5 had no time losses, crashes, disconnects or illegal moves in 1,060
  games, and produced no fastchess warnings at all. (The one time forfeit in the run was
  by Stash 20.)

### Series standings

| Rank | Engine | Elo | 95% CI | Games |
|---:|---|---:|:---:|---:|
| 1 | Opus 5.5 chess 24hrs | 3463 | ±16 | 2,180 |
| 2 | Sonnet 5.5 chess 24hrs | 3420 | ±16 | 2,180 |
| 3 | Fable 5.1 chess 24hrs | 3260 | ±19 | 1,580 |
| 4 | Opus 5 chess 24hrs | 3229 | ±19 | 1,580 |
| 5 | Astra 6 chess 24hrs | 3144 | ±20 | 1,520 |
| 6 | Fable 5 chess 24hrs | 3045 | ±21 | 1,300 |
| **7** | **Haiku 5.5 chess 24hrs** | **2765** | **±24** | **1,060** |
| 8 | Sonnet 5 chess 24hrs | 2703 | ±23 | 1,360 |

### Haiku 5.5's individual results

| Opponent | Rating | Games | +W | =D | −L | Score | Implied Elo |
|---|---:|---:|---:|---:|---:|---:|---:|
| Fable 5 (AI) | 3045 | 100 | 9 | 12 | 79 | 15.0% | 2744 |
| Crafty 25.6 | 2970 | 160 | 30 | 35 | 95 | 29.7% | 2820 |
| Stash 25 | 2932 | 160 | 27 | 36 | 97 | 28.1% | 2769 |
| Juggernaut 2.01 | 2760 | 160 | 64 | 20 | 76 | 46.2% | 2734 |
| Stash 21 | 2714 | 160 | 83 | 24 | 53 | 59.4% | 2780 |
| Sonnet 5 (AI) | 2703 | 160 | 65 | 56 | 39 | 58.1% | 2760 |
| Stash 20 | 2512 | 160 | 110 | 31 | 19 | 78.4% | 2736 |

Anchor ratings are CCRL Blitz (2'+1") values from 2 Sep 2026. AI-engine ratings are their
fitted values from the same run. The ±24 interval is statistical only. It does not include
the uncertainty in transferring CCRL ratings to 10+0.1 on this machine, so the result is a
rating "on the CCRL Blitz scale under 10+0.1 conditions", not a CCRL rating.
