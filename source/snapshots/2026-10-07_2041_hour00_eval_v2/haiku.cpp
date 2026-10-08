// Haiku 5.5 chess 24hrs: a UCI chess engine written from scratch (C++17, standard library only).
//
// Board: bitboards plus a mailbox array. Moves are generated pseudo-legally and checked for
// legality by make/unmake. Sliding attacks use PEXT-indexed tables (BMI2).
// Search: iterative deepening, PVS with aspiration windows, transposition table, null move,
// late move reductions, killer and history ordering, check extension, quiescence with SEE.
// Evaluation: Simplified Evaluation Function piece-square tables, tapered by game phase.
// Input is read on a separate thread so that stop/isready/quit work during a search.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <immintrin.h>

namespace {

using U64 = uint64_t;
using Move = uint32_t;

constexpr const char* ENGINE_NAME = "Haiku 5.5 chess 24hrs";
constexpr const char* ENGINE_AUTHOR = "Haiku 5.5";
constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

constexpr int WHITE = 0, BLACK = 1;
constexpr int PAWN = 0, KNIGHT = 1, BISHOP = 2, ROOK = 3, QUEEN = 4, KING = 5;
constexpr int NO_PIECE = 12;
constexpr int CR_WK = 1, CR_WQ = 2, CR_BK = 4, CR_BQ = 8;
constexpr uint32_t MF_EP = 1, MF_CASTLE = 2;

// Feature switches (build with -DNAME=0 or -DNAME=1 to compare variants).
#ifndef EVAL_MOBILITY
#define EVAL_MOBILITY 0
#endif
#ifndef SEARCH_IMPROVING
#define SEARCH_IMPROVING 0
#endif

constexpr int MAX_PLY = 128;
constexpr int MAX_GAME = 2048;
constexpr int INF = 32000;
constexpr int MATE = 30000;
constexpr int MATE_BOUND = MATE - MAX_PLY;
constexpr int TEMPO = 10;
constexpr int PIECE_VAL[6] = {100, 320, 330, 500, 900, 20000};
constexpr int PHASE_W[6] = {0, 1, 1, 2, 4, 0};

constexpr U64 RANK_1 = 0x00000000000000FFULL;
constexpr U64 RANK_2 = 0x000000000000FF00ULL;
constexpr U64 RANK_7 = 0x00FF000000000000ULL;
constexpr U64 RANK_8 = 0xFF00000000000000ULL;
constexpr U64 FILE_A = 0x0101010101010101ULL;
constexpr U64 FILE_H = 0x8080808080808080ULL;
constexpr U64 EDGES = RANK_1 | RANK_8 | FILE_A | FILE_H;

enum : uint8_t { BOUND_EXACT = 1, BOUND_LOWER = 2, BOUND_UPPER = 3 };

// ---------------------------------------------------------------- move encoding
// bits 0-5 from, 6-11 to, 12-14 promotion piece (KNIGHT..QUEEN), 16-17 flags
inline int mFrom(Move m) { return m & 63; }
inline int mTo(Move m) { return (m >> 6) & 63; }
inline int mPromo(Move m) { return (m >> 12) & 7; }
inline uint32_t mFlags(Move m) { return m >> 16; }
inline Move enc(int from, int to, int promo, uint32_t flags) {
    return (Move)(from | (to << 6) | (promo << 12) | (flags << 16));
}

struct ScoredMove {
    Move m;
    int s;
};
struct MoveList {
    ScoredMove mv[256];
    int n = 0;
    void add(Move m) { mv[n].m = m; mv[n].s = 0; n++; }
};

// ---------------------------------------------------------------- attack tables
U64 KNIGHT_ATT[64], KING_ATT[64], PAWN_ATT[2][64];

struct SliderInfo {
    U64 mask;
    int off;
};
SliderInfo ROOK_INFO[64], BISHOP_INFO[64];
U64 ATT[107648];  // 102400 rook + 5248 bishop entries

const int ROOK_DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int BISHOP_DIRS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

U64 slideAttacks(int sq, U64 occ, const int (*dirs)[2]) {
    U64 a = 0;
    const int r0 = sq >> 3, f0 = sq & 7;
    for (int d = 0; d < 4; d++) {
        int r = r0 + dirs[d][0], f = f0 + dirs[d][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            const U64 b = 1ULL << (r * 8 + f);
            a |= b;
            if (occ & b) break;
            r += dirs[d][0];
            f += dirs[d][1];
        }
    }
    return a;
}

inline U64 rookAtt(int sq, U64 occ) {
    const SliderInfo& s = ROOK_INFO[sq];
    return ATT[s.off + _pext_u64(occ, s.mask)];
}
inline U64 bishopAtt(int sq, U64 occ) {
    const SliderInfo& s = BISHOP_INFO[sq];
    return ATT[s.off + _pext_u64(occ, s.mask)];
}

void initAttacks() {
    const int knightOff[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
    for (int sq = 0; sq < 64; sq++) {
        const int r = sq >> 3, f = sq & 7;
        KNIGHT_ATT[sq] = KING_ATT[sq] = PAWN_ATT[WHITE][sq] = PAWN_ATT[BLACK][sq] = 0;
        for (const auto& o : knightOff) {
            const int rr = r + o[0], ff = f + o[1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KNIGHT_ATT[sq] |= 1ULL << (rr * 8 + ff);
        }
        for (int dr = -1; dr <= 1; dr++)
            for (int df = -1; df <= 1; df++) {
                if (!dr && !df) continue;
                const int rr = r + dr, ff = f + df;
                if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KING_ATT[sq] |= 1ULL << (rr * 8 + ff);
            }
        for (int df = -1; df <= 1; df += 2) {
            const int ff = f + df;
            if (ff < 0 || ff > 7) continue;
            if (r < 7) PAWN_ATT[WHITE][sq] |= 1ULL << ((r + 1) * 8 + ff);
            if (r > 0) PAWN_ATT[BLACK][sq] |= 1ULL << ((r - 1) * 8 + ff);
        }
    }
}

void initSliders() {
    int off = 0;
    for (int sq = 0; sq < 64; sq++) {
        // Rook: drop the far end of each ray (the edge square beyond the rook's own edge).
        const int r = sq >> 3, f = sq & 7;
        U64 m = slideAttacks(sq, 0, ROOK_DIRS);
        if (r != 0) m &= ~RANK_1;
        if (r != 7) m &= ~RANK_8;
        if (f != 0) m &= ~FILE_A;
        if (f != 7) m &= ~FILE_H;
        ROOK_INFO[sq] = {m, off};
        U64 sub = 0;
        do {
            ATT[off + _pext_u64(sub, m)] = slideAttacks(sq, sub, ROOK_DIRS);
            sub = (sub - m) & m;
        } while (sub);
        off += 1 << __builtin_popcountll(m);
    }
    for (int sq = 0; sq < 64; sq++) {
        const U64 m = slideAttacks(sq, 0, BISHOP_DIRS) & ~EDGES;
        BISHOP_INFO[sq] = {m, off};
        U64 sub = 0;
        do {
            ATT[off + _pext_u64(sub, m)] = slideAttacks(sq, sub, BISHOP_DIRS);
            sub = (sub - m) & m;
        } while (sub);
        off += 1 << __builtin_popcountll(m);
    }
}

// ---------------------------------------------------------------- Zobrist keys
U64 Z_PIECE[12][64], Z_CASTLE[16], Z_EP[8], Z_SIDE;

U64 splitmix(U64& s) {
    U64 z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void initZobrist() {
    U64 s = 0x2545F4914F6CDD1DULL;
    for (auto& row : Z_PIECE)
        for (auto& v : row) v = splitmix(s);
    for (auto& v : Z_CASTLE) v = splitmix(s);
    for (auto& v : Z_EP) v = splitmix(s);
    Z_SIDE = splitmix(s);
}

// ---------------------------------------------------------------- evaluation tables
// Simplified Evaluation Function (chessprogramming.org). Rows are listed from rank 8 down to rank 1
// as published: White uses index sq^56, Black uses index sq (mirrored).
constexpr int PST_MG[6][64] = {
    {0, 0, 0, 0, 0, 0, 0, 0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
     5, 5, 10, 25, 25, 10, 5, 5,
     0, 0, 0, 20, 20, 0, 0, 0,
     5, -5, -10, 0, 0, -10, -5, 5,
     5, 10, 10, -20, -20, 10, 10, 5,
     0, 0, 0, 0, 0, 0, 0, 0},
    {-50, -40, -30, -30, -30, -30, -40, -50,
     -40, -20, 0, 0, 0, 0, -20, -40,
     -30, 0, 10, 15, 15, 10, 0, -30,
     -30, 5, 15, 20, 20, 15, 5, -30,
     -30, 0, 15, 20, 20, 15, 0, -30,
     -30, 5, 10, 15, 15, 10, 5, -30,
     -40, -20, 0, 5, 5, 0, -20, -40,
     -50, -40, -30, -30, -30, -30, -40, -50},
    {-20, -10, -10, -10, -10, -10, -10, -20,
     -10, 0, 0, 0, 0, 0, 0, -10,
     -10, 0, 5, 10, 10, 5, 0, -10,
     -10, 5, 5, 10, 10, 5, 5, -10,
     -10, 0, 10, 10, 10, 10, 0, -10,
     -10, 10, 10, 10, 10, 10, 10, -10,
     -10, 5, 0, 0, 0, 0, 5, -10,
     -20, -10, -10, -10, -10, -10, -10, -20},
    {0, 0, 0, 0, 0, 0, 0, 0,
     5, 10, 10, 10, 10, 10, 10, 5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     0, 0, 0, 5, 5, 0, 0, 0},
    {-20, -10, -10, -5, -5, -10, -10, -20,
     -10, 0, 0, 0, 0, 0, 0, -10,
     -10, 0, 5, 5, 5, 5, 0, -10,
     -5, 0, 5, 5, 5, 5, 0, -5,
     0, 0, 5, 5, 5, 5, 0, -5,
     -10, 5, 5, 5, 5, 5, 0, -10,
     -10, 0, 5, 0, 0, 0, 0, -10,
     -20, -10, -10, -5, -5, -10, -10, -20},
    {-30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -20, -30, -30, -40, -40, -30, -30, -20,
     -10, -20, -20, -20, -20, -20, -20, -10,
     20, 20, 0, 0, 0, 0, 20, 20,
     20, 30, 10, 0, 0, 10, 30, 20},
};
constexpr int KING_EG[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50,
    -30, -20, -10, 0, 0, -10, -20, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -30, 0, 0, 0, 0, -30, -30,
    -50, -30, -30, -30, -30, -30, -30, -50};

int PSQ_MG[12][64], PSQ_EG[12][64];

void initPSQ() {
    for (int c = 0; c < 2; c++)
        for (int pt = 0; pt < 6; pt++)
            for (int sq = 0; sq < 64; sq++) {
                const int idx = c == WHITE ? (sq ^ 56) : sq;
                const int mat = pt == KING ? 0 : PIECE_VAL[pt];
                PSQ_MG[c * 6 + pt][sq] = mat + PST_MG[pt][idx];
                PSQ_EG[c * 6 + pt][sq] = mat + (pt == KING ? KING_EG[idx] : PST_MG[pt][idx]);
            }
}

// ---------------------------------------------------------------- position
struct Undo {
    U64 key;
    int castling, ep, halfmove, captured;
    int pstMG[2], pstEG[2], phase;
};

struct Position {
    U64 bb[12];
    U64 occ[2];
    uint8_t pc[64];
    int side, castling, ep, halfmove;
    U64 key;
    int pstMG[2], pstEG[2], phase;
    int gamePly;
    U64 keyHist[MAX_GAME];
    Undo undo[MAX_GAME];
};

Position pos;

uint8_t CASTLE_MASK[64];

inline U64 allOcc() { return pos.occ[0] | pos.occ[1]; }
inline int kingSq(int c) { return __builtin_ctzll(pos.bb[c * 6 + KING]); }

inline void putPiece(int sq, int p) {
    const U64 b = 1ULL << sq;
    pos.bb[p] |= b;
    pos.occ[p / 6] |= b;
    pos.pc[sq] = (uint8_t)p;
}
inline void takePiece(int sq) {
    const int p = pos.pc[sq];
    const U64 b = 1ULL << sq;
    pos.bb[p] &= ~b;
    pos.occ[p / 6] &= ~b;
    pos.pc[sq] = NO_PIECE;
}
inline void addPiece(int sq, int p) {
    putPiece(sq, p);
    pos.key ^= Z_PIECE[p][sq];
    pos.pstMG[p / 6] += PSQ_MG[p][sq];
    pos.pstEG[p / 6] += PSQ_EG[p][sq];
    pos.phase += PHASE_W[p % 6];
}
inline void removePiece(int sq) {
    const int p = pos.pc[sq];
    takePiece(sq);
    pos.key ^= Z_PIECE[p][sq];
    pos.pstMG[p / 6] -= PSQ_MG[p][sq];
    pos.pstEG[p / 6] -= PSQ_EG[p][sq];
    pos.phase -= PHASE_W[p % 6];
}

bool setFen(const std::string& fen) {
    for (auto& b : pos.bb) b = 0;
    pos.occ[0] = pos.occ[1] = 0;
    for (auto& p : pos.pc) p = NO_PIECE;
    pos.key = 0;
    pos.pstMG[0] = pos.pstMG[1] = pos.pstEG[0] = pos.pstEG[1] = 0;
    pos.phase = 0;
    pos.castling = 0;
    pos.ep = -1;
    pos.halfmove = 0;
    pos.gamePly = 0;

    std::istringstream iss(fen);
    std::string board, side = "w", castle = "-", ep = "-";
    int hm = 0;
    iss >> board >> side >> castle >> ep >> hm;
    if (board.empty()) return false;

    int rank = 7, file = 0;
    for (char ch : board) {
        if (ch == '/') {
            rank--;
            file = 0;
        } else if (ch >= '1' && ch <= '8') {
            file += ch - '0';
        } else {
            const int color = (ch >= 'A' && ch <= 'Z') ? WHITE : BLACK;
            int pt;
            switch (ch | 0x20) {
                case 'p': pt = PAWN; break;
                case 'n': pt = KNIGHT; break;
                case 'b': pt = BISHOP; break;
                case 'r': pt = ROOK; break;
                case 'q': pt = QUEEN; break;
                case 'k': pt = KING; break;
                default: return false;
            }
            if (rank < 0 || rank > 7 || file < 0 || file > 7) return false;
            addPiece(rank * 8 + file, color * 6 + pt);
            file++;
        }
    }
    for (char ch : castle) {
        if (ch == 'K') pos.castling |= CR_WK;
        if (ch == 'Q') pos.castling |= CR_WQ;
        if (ch == 'k') pos.castling |= CR_BK;
        if (ch == 'q') pos.castling |= CR_BQ;
    }
    if (ep.size() >= 2 && ep[0] >= 'a' && ep[0] <= 'h' && ep[1] >= '1' && ep[1] <= '8')
        pos.ep = (ep[1] - '1') * 8 + (ep[0] - 'a');
    pos.halfmove = hm < 0 ? 0 : hm;
    pos.side = (side == "b") ? BLACK : WHITE;

    pos.key ^= Z_CASTLE[pos.castling];
    if (pos.ep >= 0) pos.key ^= Z_EP[pos.ep & 7];
    if (pos.side == BLACK) pos.key ^= Z_SIDE;
    pos.keyHist[0] = pos.key;
    return pos.bb[KING] && pos.bb[6 + KING];
}

// ---------------------------------------------------------------- make / unmake
void makeMove(Move m) {
    const int from = mFrom(m), to = mTo(m), promo = mPromo(m);
    const uint32_t flags = mFlags(m);
    const int us = pos.side, them = us ^ 1;
    Undo& u = pos.undo[pos.gamePly];
    u.key = pos.key;
    u.castling = pos.castling;
    u.ep = pos.ep;
    u.halfmove = pos.halfmove;
    u.pstMG[0] = pos.pstMG[0];
    u.pstMG[1] = pos.pstMG[1];
    u.pstEG[0] = pos.pstEG[0];
    u.pstEG[1] = pos.pstEG[1];
    u.phase = pos.phase;

    const int pc = pos.pc[from];
    const int pt = pc % 6;
    int cap = NO_PIECE;
    if (flags & MF_EP) {
        const int capSq = us == WHITE ? to - 8 : to + 8;
        cap = pos.pc[capSq];
        removePiece(capSq);
    } else if (pos.pc[to] != NO_PIECE) {
        cap = pos.pc[to];
        removePiece(to);
    }
    u.captured = cap;

    if (pos.ep >= 0) {
        pos.key ^= Z_EP[pos.ep & 7];
        pos.ep = -1;
    }
    removePiece(from);
    addPiece(to, promo ? us * 6 + promo : pc);

    pos.halfmove = (pt == PAWN || cap != NO_PIECE) ? 0 : pos.halfmove + 1;

    if (flags & MF_CASTLE) {
        if (to > from) {  // king side: rook h -> f
            removePiece(to + 1);
            addPiece(to - 1, us * 6 + ROOK);
        } else {  // queen side: rook a -> d
            removePiece(to - 2);
            addPiece(to + 1, us * 6 + ROOK);
        }
    }
    if (pt == PAWN && (to - from == 16 || from - to == 16)) {
        pos.ep = (from + to) / 2;
        pos.key ^= Z_EP[pos.ep & 7];
    }

    pos.key ^= Z_CASTLE[pos.castling];
    pos.castling &= CASTLE_MASK[from] & CASTLE_MASK[to];
    pos.key ^= Z_CASTLE[pos.castling];

    pos.side = them;
    pos.key ^= Z_SIDE;
    pos.gamePly++;
    pos.keyHist[pos.gamePly] = pos.key;
}

void unmakeMove(Move m) {
    const int from = mFrom(m), to = mTo(m), promo = mPromo(m);
    const uint32_t flags = mFlags(m);
    pos.gamePly--;
    const Undo& u = pos.undo[pos.gamePly];
    pos.side ^= 1;
    const int us = pos.side;

    const int moved = promo ? us * 6 + PAWN : pos.pc[to];
    takePiece(to);
    putPiece(from, moved);
    if (u.captured != NO_PIECE) {
        const int capSq = (flags & MF_EP) ? (us == WHITE ? to - 8 : to + 8) : to;
        putPiece(capSq, u.captured);
    }
    if (flags & MF_CASTLE) {
        if (to > from) {
            takePiece(to - 1);
            putPiece(to + 1, us * 6 + ROOK);
        } else {
            takePiece(to + 1);
            putPiece(to - 2, us * 6 + ROOK);
        }
    }
    pos.key = u.key;
    pos.castling = u.castling;
    pos.ep = u.ep;
    pos.halfmove = u.halfmove;
    pos.pstMG[0] = u.pstMG[0];
    pos.pstMG[1] = u.pstMG[1];
    pos.pstEG[0] = u.pstEG[0];
    pos.pstEG[1] = u.pstEG[1];
    pos.phase = u.phase;
}

void makeNull() {
    Undo& u = pos.undo[pos.gamePly];
    u.key = pos.key;
    u.castling = pos.castling;
    u.ep = pos.ep;
    u.halfmove = pos.halfmove;
    u.captured = NO_PIECE;
    u.pstMG[0] = pos.pstMG[0];
    u.pstMG[1] = pos.pstMG[1];
    u.pstEG[0] = pos.pstEG[0];
    u.pstEG[1] = pos.pstEG[1];
    u.phase = pos.phase;
    if (pos.ep >= 0) {
        pos.key ^= Z_EP[pos.ep & 7];
        pos.ep = -1;
    }
    pos.side ^= 1;
    pos.key ^= Z_SIDE;
    pos.gamePly++;
    pos.keyHist[pos.gamePly] = pos.key;
}

void unmakeNull() {
    pos.gamePly--;
    const Undo& u = pos.undo[pos.gamePly];
    pos.side ^= 1;
    pos.key = u.key;
    pos.ep = u.ep;
    pos.castling = u.castling;
    pos.halfmove = u.halfmove;
    pos.pstMG[0] = u.pstMG[0];
    pos.pstMG[1] = u.pstMG[1];
    pos.pstEG[0] = u.pstEG[0];
    pos.pstEG[1] = u.pstEG[1];
    pos.phase = u.phase;
}

// ---------------------------------------------------------------- attacks and move generation
inline bool attackedBy(int sq, int by, U64 occ) {
    if (PAWN_ATT[by ^ 1][sq] & pos.bb[by * 6 + PAWN]) return true;
    if (KNIGHT_ATT[sq] & pos.bb[by * 6 + KNIGHT]) return true;
    if (KING_ATT[sq] & pos.bb[by * 6 + KING]) return true;
    if (bishopAtt(sq, occ) & (pos.bb[by * 6 + BISHOP] | pos.bb[by * 6 + QUEEN])) return true;
    if (rookAtt(sq, occ) & (pos.bb[by * 6 + ROOK] | pos.bb[by * 6 + QUEEN])) return true;
    return false;
}

inline U64 attackersTo(int sq, U64 occ) {
    const U64 diag = pos.bb[BISHOP] | pos.bb[6 + BISHOP] | pos.bb[QUEEN] | pos.bb[6 + QUEEN];
    const U64 orth = pos.bb[ROOK] | pos.bb[6 + ROOK] | pos.bb[QUEEN] | pos.bb[6 + QUEEN];
    return ((PAWN_ATT[BLACK][sq] & pos.bb[PAWN]) | (PAWN_ATT[WHITE][sq] & pos.bb[6 + PAWN]) |
            (KNIGHT_ATT[sq] & (pos.bb[KNIGHT] | pos.bb[6 + KNIGHT])) |
            (KING_ATT[sq] & (pos.bb[KING] | pos.bb[6 + KING])) | (bishopAtt(sq, occ) & diag) |
            (rookAtt(sq, occ) & orth)) &
           occ;
}

inline bool inCheck() { return attackedBy(kingSq(pos.side), pos.side ^ 1, allOcc()); }

inline bool hasNonPawn(int c) {
    return (pos.bb[c * 6 + KNIGHT] | pos.bb[c * 6 + BISHOP] | pos.bb[c * 6 + ROOK] | pos.bb[c * 6 + QUEEN]) != 0;
}

void addPromos(MoveList& ml, int from, int to, uint32_t flags) {
    ml.add(enc(from, to, QUEEN, flags));
    ml.add(enc(from, to, ROOK, flags));
    ml.add(enc(from, to, BISHOP, flags));
    ml.add(enc(from, to, KNIGHT, flags));
}

// Pseudo-legal generation. Legality (king not left in check) is checked by the caller.
void genMoves(MoveList& ml, bool capsOnly) {
    ml.n = 0;
    const int us = pos.side, them = us ^ 1;
    const U64 ours = pos.occ[us], theirs = pos.occ[them], all = ours | theirs;
    const int dir = us == WHITE ? 8 : -8;
    const U64 lastRank = us == WHITE ? RANK_8 : RANK_1;
    const U64 startRank = us == WHITE ? RANK_2 : RANK_7;

    U64 pawns = pos.bb[us * 6 + PAWN];
    while (pawns) {
        const int from = __builtin_ctzll(pawns);
        pawns &= pawns - 1;
        const int to = from + dir;
        if (!(all & (1ULL << to))) {
            if (lastRank & (1ULL << to)) {
                addPromos(ml, from, to, 0);
            } else if (!capsOnly) {
                ml.add(enc(from, to, 0, 0));
                const int to2 = to + dir;
                if ((startRank & (1ULL << from)) && !(all & (1ULL << to2))) ml.add(enc(from, to2, 0, 0));
            }
        }
        U64 caps = PAWN_ATT[us][from] & theirs;
        while (caps) {
            const int t = __builtin_ctzll(caps);
            caps &= caps - 1;
            if (lastRank & (1ULL << t)) addPromos(ml, from, t, 0);
            else ml.add(enc(from, t, 0, 0));
        }
        if (pos.ep >= 0 && (PAWN_ATT[us][from] & (1ULL << pos.ep))) ml.add(enc(from, pos.ep, 0, MF_EP));
    }

    for (int pt = KNIGHT; pt <= KING; pt++) {
        U64 pcs = pos.bb[us * 6 + pt];
        while (pcs) {
            const int from = __builtin_ctzll(pcs);
            pcs &= pcs - 1;
            U64 att;
            switch (pt) {
                case KNIGHT: att = KNIGHT_ATT[from]; break;
                case BISHOP: att = bishopAtt(from, all); break;
                case ROOK: att = rookAtt(from, all); break;
                case QUEEN: att = bishopAtt(from, all) | rookAtt(from, all); break;
                default: att = KING_ATT[from]; break;
            }
            att &= capsOnly ? theirs : ~ours;
            while (att) ml.add(enc(from, __builtin_ctzll(att), 0, 0)), att &= att - 1;
        }
    }

    if (!capsOnly && !attackedBy(kingSq(us), them, all)) {
        if (us == WHITE) {
            if ((pos.castling & CR_WK) && !(all & ((1ULL << 5) | (1ULL << 6))) &&
                !attackedBy(5, them, all) && !attackedBy(6, them, all))
                ml.add(enc(4, 6, 0, MF_CASTLE));
            if ((pos.castling & CR_WQ) && !(all & ((1ULL << 1) | (1ULL << 2) | (1ULL << 3))) &&
                !attackedBy(3, them, all) && !attackedBy(2, them, all))
                ml.add(enc(4, 2, 0, MF_CASTLE));
        } else {
            if ((pos.castling & CR_BK) && !(all & ((1ULL << 61) | (1ULL << 62))) &&
                !attackedBy(61, them, all) && !attackedBy(62, them, all))
                ml.add(enc(60, 62, 0, MF_CASTLE));
            if ((pos.castling & CR_BQ) && !(all & ((1ULL << 57) | (1ULL << 58) | (1ULL << 59))) &&
                !attackedBy(59, them, all) && !attackedBy(58, them, all))
                ml.add(enc(60, 58, 0, MF_CASTLE));
        }
    }
}

// ---------------------------------------------------------------- static exchange evaluation
int see(Move m) {
    const int from = mFrom(m), to = mTo(m), promo = mPromo(m);
    const bool ep = mFlags(m) & MF_EP;
    const int us = pos.side;
    const int moverPt = pos.pc[from] % 6;
    int gain[32];
    int d = 0;
    gain[0] = 0;
    if (ep) gain[0] = PIECE_VAL[PAWN];
    else if (pos.pc[to] != NO_PIECE) gain[0] = PIECE_VAL[pos.pc[to] % 6];
    if (promo) gain[0] += PIECE_VAL[promo] - PIECE_VAL[PAWN];
    int curVal = promo ? PIECE_VAL[promo] : PIECE_VAL[moverPt];

    U64 occ = allOcc() ^ (1ULL << from);
    if (ep) occ ^= 1ULL << (us == WHITE ? to - 8 : to + 8);
    U64 attackers = attackersTo(to, occ);
    int side = us ^ 1;
    while (d < 31) {
        int pt = -1;
        U64 cand = 0;
        for (int t = PAWN; t <= KING; t++) {
            const U64 b = attackers & pos.bb[side * 6 + t];
            if (b) {
                pt = t;
                cand = b;
                break;
            }
        }
        if (pt < 0) break;
        d++;
        gain[d] = curVal - gain[d - 1];
        occ ^= cand & (0 - cand);
        attackers = attackersTo(to, occ) & occ;
        curVal = PIECE_VAL[pt];
        side ^= 1;
    }
    while (d > 0) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
        d--;
    }
    return gain[0];
}

// ---------------------------------------------------------------- evaluation
U64 PASSED_MASK[2][64];   // squares ahead of a pawn of that colour on its own and adjacent files
U64 ADJACENT_FILES[8];
U64 FILE_MASK[8];

inline int popcount_(U64 b) { return __builtin_popcountll(b); }

void initEvalMasks() {
    for (int f = 0; f < 8; f++) {
        FILE_MASK[f] = FILE_A << f;
        ADJACENT_FILES[f] = (f > 0 ? FILE_A << (f - 1) : 0) | (f < 7 ? FILE_A << (f + 1) : 0);
    }
    for (int sq = 0; sq < 64; sq++) {
        const int r = sq >> 3, f = sq & 7;
        U64 wm = 0, bm = 0;
        for (int rr = 0; rr < 8; rr++)
            for (int ff = std::max(0, f - 1); ff <= std::min(7, f + 1); ff++) {
                if (rr > r) wm |= 1ULL << (rr * 8 + ff);
                if (rr < r) bm |= 1ULL << (rr * 8 + ff);
            }
        PASSED_MASK[WHITE][sq] = wm;
        PASSED_MASK[BLACK][sq] = bm;
    }
}

// Piece mobility: squares a piece attacks that are not its own and not covered by enemy pawns.
void mobilityTerms(int c, U64 enemyPawnAtt, int& mg, int& eg) {
    const U64 all = allOcc();
    const U64 safe = ~pos.occ[c] & ~enemyPawnAtt;
    U64 pcs = pos.bb[c * 6 + KNIGHT];
    while (pcs) {
        const int n = popcount_(KNIGHT_ATT[__builtin_ctzll(pcs)] & safe);
        pcs &= pcs - 1;
        mg += (n - 4) * 4;
        eg += (n - 4) * 4;
    }
    pcs = pos.bb[c * 6 + BISHOP];
    while (pcs) {
        const int n = popcount_(bishopAtt(__builtin_ctzll(pcs), all) & safe);
        pcs &= pcs - 1;
        mg += (n - 7) * 3;
        eg += (n - 7) * 4;
    }
    pcs = pos.bb[c * 6 + ROOK];
    while (pcs) {
        const int n = popcount_(rookAtt(__builtin_ctzll(pcs), all) & safe);
        pcs &= pcs - 1;
        mg += (n - 7) * 2;
        eg += (n - 7) * 4;
    }
    pcs = pos.bb[c * 6 + QUEEN];
    while (pcs) {
        const int sq = __builtin_ctzll(pcs);
        pcs &= pcs - 1;
        const int n = popcount_((bishopAtt(sq, all) | rookAtt(sq, all)) & safe);
        mg += (n - 14);
        eg += (n - 14) * 2;
    }
}

// Pawn structure and piece-placement terms, added to the middlegame and endgame sums.
void extraTerms(int c, int& mg, int& eg) {
    const int them = c ^ 1;
    const U64 own = pos.bb[c * 6 + PAWN], enemy = pos.bb[them * 6 + PAWN];
    if (popcount_(pos.bb[c * 6 + BISHOP]) >= 2) {
        mg += 30;
        eg += 50;
    }
    static const int PASSED_MG[8] = {0, 0, 4, 8, 16, 28, 48, 0};
    static const int PASSED_EG[8] = {0, 0, 8, 16, 30, 52, 90, 0};
    U64 pawns = own;
    while (pawns) {
        const int sq = __builtin_ctzll(pawns);
        pawns &= pawns - 1;
        const int f = sq & 7;
        const int rel = c == WHITE ? (sq >> 3) : 7 - (sq >> 3);
        if (!(own & ADJACENT_FILES[f])) {
            mg -= 10;
            eg -= 15;  // isolated
        }
        if (!(enemy & PASSED_MASK[c][sq])) {
            mg += PASSED_MG[rel];
            eg += PASSED_EG[rel];
        }
    }
    for (int f = 0; f < 8; f++) {
        const int n = popcount_(own & FILE_MASK[f]);
        if (n > 1) {  // doubled pawns
            mg -= 8 * (n - 1);
            eg -= 16 * (n - 1);
        }
    }
    U64 rooks = pos.bb[c * 6 + ROOK];
    while (rooks) {
        const int f = __builtin_ctzll(rooks) & 7;
        rooks &= rooks - 1;
        if (!(own & FILE_MASK[f])) {
            mg += (enemy & FILE_MASK[f]) ? 8 : 16;  // semi-open or open file
            eg += (enemy & FILE_MASK[f]) ? 4 : 8;
        }
    }
}

int evaluate() {
    const int ph = std::min(pos.phase, 24);
    int mg = pos.pstMG[WHITE] - pos.pstMG[BLACK];
    int eg = pos.pstEG[WHITE] - pos.pstEG[BLACK];
    int wmg = 0, weg = 0, bmg = 0, beg = 0;
    extraTerms(WHITE, wmg, weg);
    extraTerms(BLACK, bmg, beg);
    mg += wmg - bmg;
    eg += weg - beg;
#if EVAL_MOBILITY
    U64 wPawnAtt = 0, bPawnAtt = 0;
    for (U64 b = pos.bb[PAWN]; b; b &= b - 1) wPawnAtt |= PAWN_ATT[WHITE][__builtin_ctzll(b)];
    for (U64 b = pos.bb[6 + PAWN]; b; b &= b - 1) bPawnAtt |= PAWN_ATT[BLACK][__builtin_ctzll(b)];
    wmg = weg = bmg = beg = 0;
    mobilityTerms(WHITE, bPawnAtt, wmg, weg);
    mobilityTerms(BLACK, wPawnAtt, bmg, beg);
    mg += wmg - bmg;
    eg += weg - beg;
#endif
    const int s = (mg * ph + eg * (24 - ph)) / 24;
    return (pos.side == WHITE ? s : -s) + TEMPO;
}

// ---------------------------------------------------------------- transposition table
struct TTEntry {
    U64 key;
    uint16_t move;
    int16_t score;
    int8_t depth;
    uint8_t bound;
    uint8_t age;
    uint8_t pad;
};
static_assert(sizeof(TTEntry) == 16, "TT entry must be 16 bytes");

std::vector<TTEntry> g_tt;
uint8_t g_ttAge = 0;

inline size_t ttIndex(U64 key) { return (size_t)(((unsigned __int128)key * g_tt.size()) >> 64); }

void ttResize(int mb) {
    const size_t n = (size_t)mb * 1024 * 1024 / sizeof(TTEntry);
    g_tt.assign(n > 0 ? n : 1, TTEntry{});
}

int ttHashfull() {
    const size_t lim = std::min<size_t>(1000, g_tt.size());
    int n = 0;
    for (size_t i = 0; i < lim; i++)
        if (g_tt[i].key && g_tt[i].age == g_ttAge) n++;
    return lim ? n * 1000 / (int)lim : 0;
}

int scoreToTT(int s, int ply) {
    if (s >= MATE_BOUND) return s + ply;
    if (s <= -MATE_BOUND) return s - ply;
    return s;
}
int scoreFromTT(int s, int ply) {
    if (s >= MATE_BOUND) return s - ply;
    if (s <= -MATE_BOUND) return s + ply;
    return s;
}

void ttStore(U64 key, Move m, int score, int depth, int bound, int ply) {
    TTEntry& e = g_tt[ttIndex(key)];
    if (e.key != key || bound == BOUND_EXACT || depth + 3 >= e.depth || e.age != g_ttAge) {
        if (m || e.key != key) e.move = (uint16_t)(m & 0xFFFF);
        e.key = key;
        e.score = (int16_t)scoreToTT(score, ply);
        e.depth = (int8_t)depth;
        e.bound = (uint8_t)bound;
        e.age = g_ttAge;
    }
}

// ---------------------------------------------------------------- search state
std::atomic<bool> g_stopSignal{false};
std::atomic<bool> g_searching{false};  // set by the input thread on "go", cleared when the search ends
bool g_abort = false;        // search is being abandoned
bool g_allowAbort = false;   // at least one iteration finished, so time limits may abort
bool g_timed = false;
double g_softMs = 0, g_hardMs = 0;
int g_moveOverhead = 30;
U64 g_nodes = 0;
int g_seldepth = 0;

using Clock = std::chrono::steady_clock;
Clock::time_point g_start;

inline double elapsedMs() {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

constexpr int NO_EVAL = -INF - 1;  // marks a ply where the side to move is in check
int g_evalStk[MAX_PLY];
Move g_killers[MAX_PLY][2];
int g_history[2][64][64];
Move g_pv[MAX_PLY][MAX_PLY];
int g_pvLen[MAX_PLY];
int g_lmr[64][64];

inline void pollAbort() {
    if (g_stopSignal.load(std::memory_order_relaxed)) g_abort = true;
    else if (g_timed && g_allowAbort && elapsedMs() >= g_hardMs) g_abort = true;
}

bool isRepetition() {
    const int lim = std::max(0, pos.gamePly - pos.halfmove);
    for (int i = pos.gamePly - 2; i >= lim; i -= 2)
        if (pos.keyHist[i] == pos.key) return true;
    return false;
}

inline void pickBest(MoveList& ml, int start) {
    int bi = start;
    for (int j = start + 1; j < ml.n; j++)
        if (ml.mv[j].s > ml.mv[bi].s) bi = j;
    std::swap(ml.mv[start], ml.mv[bi]);
}

void scoreMoves(MoveList& ml, uint32_t ttMove16, int ply, int us) {
    for (int i = 0; i < ml.n; i++) {
        const Move m = ml.mv[i].m;
        const int from = mFrom(m), to = mTo(m), promo = mPromo(m);
        const bool ep = mFlags(m) & MF_EP;
        const bool cap = ep || pos.pc[to] != NO_PIECE;
        int s;
        if (ttMove16 && (m & 0xFFFF) == ttMove16) {
            s = 10000000;
        } else if (promo) {
            s = 6000000 + PIECE_VAL[promo];
        } else if (cap) {
            const int victim = ep ? PAWN : pos.pc[to] % 6;
            const int attacker = pos.pc[from] % 6;
            s = (see(m) >= 0 ? 4000000 : -1000000) + PIECE_VAL[victim] / 10 * 8 - attacker;
        } else if (m == g_killers[ply][0]) {
            s = 3000000;
        } else if (m == g_killers[ply][1]) {
            s = 2999999;
        } else {
            s = g_history[us][from][to];
        }
        ml.mv[i].s = s;
    }
}

void updatePV(int ply, Move m) {
    g_pv[ply][0] = m;
    const int n = g_pvLen[ply + 1];
    for (int j = 0; j < n; j++) g_pv[ply][j + 1] = g_pv[ply + 1][j];
    g_pvLen[ply] = n + 1;
}

void updateHistory(int us, Move m, int bonus) {
    int& h = g_history[us][mFrom(m)][mTo(m)];
    h += bonus * 32 - h * std::abs(bonus) / 512;
}

// ---------------------------------------------------------------- quiescence
int qsearch(int alpha, int beta, int ply) {
    if (((++g_nodes) & 1023) == 0) pollAbort();
    if (g_abort) return 0;
    if (ply > g_seldepth) g_seldepth = ply;
    if (ply >= MAX_PLY - 1) return evaluate();

    const int us = pos.side, them = us ^ 1;
    const bool chk = inCheck();
    int standPat = -INF;
    if (!chk) {
        standPat = evaluate();
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;
    }

    MoveList ml;
    genMoves(ml, !chk);
    scoreMoves(ml, 0, ply, us);

    int best = chk ? -INF : standPat;
    int legal = 0;
    for (int i = 0; i < ml.n; i++) {
        pickBest(ml, i);
        const Move m = ml.mv[i].m;
        const bool promo = mPromo(m) != 0;
        if (!chk && !promo) {
            const bool ep = mFlags(m) & MF_EP;
            const int victim = ep ? PAWN : pos.pc[mTo(m)] % 6;
            if (standPat + PIECE_VAL[victim] + 200 <= alpha) continue;
            if (see(m) < 0) continue;
        }
        makeMove(m);
        if (attackedBy(kingSq(us), them, allOcc())) {
            unmakeMove(m);
            continue;
        }
        legal++;
        const int score = -qsearch(-beta, -alpha, ply + 1);
        unmakeMove(m);
        if (g_abort) return 0;
        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) return score;
            }
        }
    }
    if (chk && legal == 0) return -MATE + ply;
    return best;
}

// ---------------------------------------------------------------- main search
int search(int alpha, int beta, int depth, int ply, bool allowNull) {
    const bool pvNode = beta - alpha > 1;
    g_pvLen[ply] = 0;
    if (ply > g_seldepth) g_seldepth = ply;
    if (((++g_nodes) & 1023) == 0) pollAbort();
    if (g_abort) return 0;

    if (ply > 0) {
        if (isRepetition() || pos.halfmove >= 100) return 0;
        alpha = std::max(alpha, -MATE + ply);
        beta = std::min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }
    if (ply >= MAX_PLY - 1) return evaluate();

    const int us = pos.side, them = us ^ 1;
    const bool chk = inCheck();
    if (chk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);

    const U64 key = pos.key;
    const TTEntry& te = g_tt[ttIndex(key)];
    const bool ttHit = te.key == key;
    uint32_t ttMove = 0;
    if (ttHit) {
        ttMove = te.move;
        if (ply > 0 && te.depth >= depth) {
            const int s = scoreFromTT(te.score, ply);
            if (te.bound == BOUND_EXACT || (te.bound == BOUND_LOWER && s >= beta) ||
                (te.bound == BOUND_UPPER && s <= alpha))
                return s;
        }
    }

    int staticEval = 0;
    if (!chk) {
        staticEval = evaluate();
        g_evalStk[ply] = staticEval;
        if (!pvNode) {
            if (depth <= 6 && staticEval - 80 * depth >= beta) return staticEval;
            if (allowNull && depth >= 3 && staticEval >= beta && hasNonPawn(us)) {
                const int R = 3 + depth / 4 + std::min(3, (staticEval - beta) / 200);
                makeNull();
                const int s = -search(-beta, -beta + 1, depth - 1 - R, ply + 1, false);
                unmakeNull();
                if (g_abort) return 0;
                if (s >= beta) return s >= MATE_BOUND ? beta : s;
            }
        }
    }

#if SEARCH_IMPROVING
    // "Improving": the side to move has a better static eval than it had two plies ago.
    const bool improving = !chk && ply >= 2 && g_evalStk[ply - 2] != NO_EVAL && staticEval > g_evalStk[ply - 2];
    if (chk) g_evalStk[ply] = NO_EVAL;
#else
    [[maybe_unused]] const bool improving = true;  // neutral: no late-move pruning change, no extra reduction
#endif

    MoveList ml;
    genMoves(ml, false);
    scoreMoves(ml, ttMove, ply, us);

    int best = -INF, legal = 0;
    Move bestMove = 0;
    const int origAlpha = alpha;
    Move quiets[64];
    int nq = 0;

    for (int i = 0; i < ml.n; i++) {
        pickBest(ml, i);
        const Move m = ml.mv[i].m;
        const int to = mTo(m);
        const bool isCap = pos.pc[to] != NO_PIECE || (mFlags(m) & MF_EP);
        const bool quiet = !isCap && mPromo(m) == 0;

        if (quiet && !chk && legal > 0 && depth <= 4 && staticEval + 110 * depth <= alpha) continue;
#if SEARCH_IMPROVING
        if (quiet && !chk && legal > 0 && depth <= 4 && legal >= (improving ? 5 : 3) + depth * depth) continue;
#endif

        makeMove(m);
        if (attackedBy(kingSq(us), them, allOcc())) {
            unmakeMove(m);
            continue;
        }
        legal++;
        const bool gives = attackedBy(kingSq(them), us, allOcc());
        const int newDepth = depth - 1;
        int score;
        if (legal == 1) {
            score = -search(-beta, -alpha, newDepth, ply + 1, true);
        } else {
            int red = 0;
            if (depth >= 3 && quiet && legal > 2 && !chk && !gives) {
                red = g_lmr[std::min(depth, 63)][std::min(legal, 63)];
                if (pvNode) red--;
#if SEARCH_IMPROVING
                if (!improving) red++;
#endif
                red = std::max(0, std::min(red, newDepth - 1));
            }
            score = -search(-alpha - 1, -alpha, newDepth - red, ply + 1, true);
            if (score > alpha && red > 0) score = -search(-alpha - 1, -alpha, newDepth, ply + 1, true);
            if (score > alpha && score < beta) score = -search(-beta, -alpha, newDepth, ply + 1, true);
        }
        unmakeMove(m);
        if (g_abort) return 0;

        if (score > best) {
            best = score;
            bestMove = m;
            if (score > alpha) {
                alpha = score;
                updatePV(ply, m);
                if (score >= beta) {
                    if (quiet) {
                        if (g_killers[ply][0] != m) {
                            g_killers[ply][1] = g_killers[ply][0];
                            g_killers[ply][0] = m;
                        }
                        const int bonus = std::min(depth * depth, 400);
                        updateHistory(us, m, bonus);
                        for (int j = 0; j < nq; j++) updateHistory(us, quiets[j], -bonus);
                    }
                    break;
                }
            }
        }
        if (quiet && nq < 64) quiets[nq++] = m;
    }

    if (legal == 0) return chk ? -MATE + ply : 0;
    const int bound = best >= beta ? BOUND_LOWER : (alpha > origAlpha ? BOUND_EXACT : BOUND_UPPER);
    ttStore(key, bestMove, best, depth, bound, ply);
    return best;
}

// ---------------------------------------------------------------- output
std::mutex g_outMutex;
void out(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_outMutex);
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
}

std::string moveToString(Move m) {
    static const char promoCh[] = " nbrq";
    std::string s;
    s += (char)('a' + (mFrom(m) & 7));
    s += (char)('1' + (mFrom(m) >> 3));
    s += (char)('a' + (mTo(m) & 7));
    s += (char)('1' + (mTo(m) >> 3));
    if (mPromo(m)) s += promoCh[mPromo(m)];
    return s;
}

std::string scoreString(int score) {
    if (score >= MATE_BOUND) return "mate " + std::to_string((MATE - score + 1) / 2);
    if (score <= -MATE_BOUND) return "mate " + std::to_string(-((MATE + score + 1) / 2));
    return "cp " + std::to_string(score);
}

void printInfo(int depth, int score) {
    const long long ms = std::max<long long>(1, (long long)elapsedMs());
    std::ostringstream os;
    os << "info depth " << depth << " seldepth " << g_seldepth << " score " << scoreString(score)
       << " nodes " << g_nodes << " nps " << (g_nodes * 1000 / (U64)ms) << " time " << ms
       << " hashfull " << ttHashfull() << " pv";
    for (int i = 0; i < g_pvLen[0]; i++) os << ' ' << moveToString(g_pv[0][i]);
    os << '\n';
    out(os.str());
}

// ---------------------------------------------------------------- move parsing and legality helpers
Move firstLegal() {
    MoveList ml;
    genMoves(ml, false);
    const int us = pos.side;
    for (int i = 0; i < ml.n; i++) {
        makeMove(ml.mv[i].m);
        const bool ok = !attackedBy(kingSq(us), us ^ 1, allOcc());
        unmakeMove(ml.mv[i].m);
        if (ok) return ml.mv[i].m;
    }
    return 0;
}

bool parseMove(const std::string& s, Move& outMove) {
    if (s.size() < 4) return false;
    const int from = (s[0] - 'a') + 8 * (s[1] - '1');
    const int to = (s[2] - 'a') + 8 * (s[3] - '1');
    int promo = 0;
    if (s.size() >= 5) {
        switch (s[4]) {
            case 'n': promo = KNIGHT; break;
            case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK; break;
            case 'q': promo = QUEEN; break;
            default: return false;
        }
    }
    MoveList ml;
    genMoves(ml, false);
    const int us = pos.side;
    for (int i = 0; i < ml.n; i++) {
        const Move m = ml.mv[i].m;
        if (mFrom(m) != from || mTo(m) != to || mPromo(m) != promo) continue;
        makeMove(m);
        const bool ok = !attackedBy(kingSq(us), us ^ 1, allOcc());
        unmakeMove(m);
        if (ok) {
            outMove = m;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- time management
struct Limits {
    bool infinite = false;
    int depth = 0;
    long long movetime = -1;
    long long wtime = -1, btime = -1, winc = 0, binc = 0;
    int movestogo = 0;
};

void setupTime(const Limits& L) {
    g_timed = false;
    g_softMs = g_hardMs = 1e18;
    if (L.infinite) return;
    if (L.movetime >= 0) {
        const double t = std::max(1.0, (double)L.movetime - g_moveOverhead);
        g_softMs = g_hardMs = t;
        g_timed = true;
        return;
    }
    const bool haveClock = L.wtime >= 0 || L.btime >= 0;
    if (!haveClock) return;
    const double my = (double)(pos.side == WHITE ? L.wtime : L.btime);
    const double inc = (double)(pos.side == WHITE ? L.winc : L.binc);
    const double avail = std::max(1.0, my - g_moveOverhead);
    const double mtg = L.movestogo > 0 ? std::min(L.movestogo, 40) : 30.0;
    double soft = avail / mtg + inc * 0.75;
    soft = std::min(soft, avail * 0.5);
    double hard = std::min(avail * 0.25 + inc, soft * 2.5);
    g_softMs = std::max(1.0, soft);
    g_hardMs = std::max(g_softMs, hard);
    g_timed = true;
}

// ---------------------------------------------------------------- iterative deepening
void think(const Limits& lim) {
    g_nodes = 0;
    g_abort = false;
    g_allowAbort = false;
    g_seldepth = 0;
    g_ttAge++;
    g_start = Clock::now();
    setupTime(lim);

    const int maxDepth = lim.depth > 0 ? std::min(lim.depth, MAX_PLY - 2) : MAX_PLY - 2;
    const Move fallback = firstLegal();
    Move bestMove = 0;
    int prevScore = 0;

    for (int depth = 1; depth <= maxDepth; depth++) {
        g_seldepth = 0;
        int score;
        if (depth >= 5) {
            int delta = 20;
            int alpha = std::max(-INF, prevScore - delta);
            int beta = std::min(INF, prevScore + delta);
            while (true) {
                score = search(alpha, beta, depth, 0, true);
                if (g_abort) break;
                if (score <= alpha) {
                    beta = (alpha + beta) / 2;
                    alpha = std::max(-INF, score - delta);
                } else if (score >= beta) {
                    beta = std::min(INF, score + delta);
                } else {
                    break;
                }
                delta += delta / 2;
            }
        } else {
            score = search(-INF, INF, depth, 0, true);
        }
        if (g_abort) break;

        if (g_pvLen[0] > 0) bestMove = g_pv[0][0];
        prevScore = score;
        g_allowAbort = true;
        printInfo(depth, score);
        if (g_timed && elapsedMs() >= g_softMs) break;
    }

    if (!bestMove) bestMove = fallback;
    out("bestmove " + (bestMove ? moveToString(bestMove) : std::string("0000")) + "\n");
    g_searching = false;
}

// ---------------------------------------------------------------- perft (testing)
U64 perft(int depth) {
    if (depth == 0) return 1;
    MoveList ml;
    genMoves(ml, false);
    U64 n = 0;
    const int us = pos.side;
    for (int i = 0; i < ml.n; i++) {
        makeMove(ml.mv[i].m);
        if (!attackedBy(kingSq(us), us ^ 1, allOcc())) n += depth == 1 ? 1 : perft(depth - 1);
        unmakeMove(ml.mv[i].m);
    }
    return n;
}

int runPerftSuite(const char* path, int maxDepth) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::string line;
    int positions = 0, failures = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t semi = line.find(';', start);
            parts.push_back(line.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        if (!setFen(parts[0])) {
            std::printf("bad fen: %s\n", parts[0].c_str());
            failures++;
            continue;
        }
        positions++;
        for (size_t i = 1; i < parts.size(); i++) {
            int d = 0;
            unsigned long long expected = 0;
            if (std::sscanf(parts[i].c_str(), " D%d %llu", &d, &expected) != 2) continue;
            if (d > maxDepth) continue;
            const U64 got = perft(d);
            if (got != expected) {
                failures++;
                std::printf("FAIL %s D%d expected %llu got %llu\n", parts[0].c_str(), d, expected,
                            (unsigned long long)got);
            }
        }
    }
    std::printf("positions %d, failures %d\n", positions, failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- UCI
void hashResize(int mb) { ttResize(mb); }

void processCommand(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return;

    if (cmd == "uci") {
        std::ostringstream os;
        os << "id name " << ENGINE_NAME << "\n"
           << "id author " << ENGINE_AUTHOR << "\n"
           << "option name Hash type spin default 256 min 1 max 2048\n"
           << "option name Move Overhead type spin default 30 min 0 max 5000\n"
           << "uciok\n";
        out(os.str());
    } else if (cmd == "isready") {
        out("readyok\n");
    } else if (cmd == "ucinewgame") {
        std::fill(g_tt.begin(), g_tt.end(), TTEntry{});
        std::memset(g_killers, 0, sizeof(g_killers));
        std::memset(g_history, 0, sizeof(g_history));
        setFen(START_FEN);
    } else if (cmd == "setoption") {
        std::string tok, name, value;
        iss >> tok;  // "name"
        bool inValue = false;
        while (iss >> tok) {
            if (!inValue && tok == "value") {
                inValue = true;
                continue;
            }
            std::string& dst = inValue ? value : name;
            dst += (dst.empty() ? "" : " ") + tok;
        }
        if (name == "Hash") {
            try {
                hashResize(std::max(1, std::min(2048, std::stoi(value))));
            } catch (...) {
            }
        } else if (name == "Move Overhead") {
            try {
                g_moveOverhead = std::max(0, std::min(5000, std::stoi(value)));
            } catch (...) {
            }
        }
    } else if (cmd == "position") {
        std::string tok;
        bool gotMoves = false;
        iss >> tok;
        if (tok == "startpos") {
            setFen(START_FEN);
            if (iss >> tok) gotMoves = tok == "moves";
        } else if (tok == "fen") {
            std::string fen;
            while (iss >> tok) {
                if (tok == "moves") {
                    gotMoves = true;
                    break;
                }
                fen += tok + " ";
            }
            setFen(fen);
        }
        if (gotMoves) {
            while (iss >> tok) {
                Move m;
                if (parseMove(tok, m)) makeMove(m);
            }
        }
    } else if (cmd == "go") {
        Limits lim;
        bool any = false;
        std::string tok;
        while (iss >> tok) {
            any = true;
            if (tok == "wtime") iss >> lim.wtime;
            else if (tok == "btime") iss >> lim.btime;
            else if (tok == "winc") iss >> lim.winc;
            else if (tok == "binc") iss >> lim.binc;
            else if (tok == "movestogo") iss >> lim.movestogo;
            else if (tok == "depth") iss >> lim.depth;
            else if (tok == "movetime") iss >> lim.movetime;
            else if (tok == "infinite") lim.infinite = true;
            else if (tok == "nodes" || tok == "mate") {
                long long ignored;
                iss >> ignored;
            }
        }
        if (!any || (lim.wtime < 0 && lim.btime < 0 && lim.movetime < 0 && lim.depth <= 0)) lim.infinite = true;
        think(lim);
    } else if (cmd == "perft") {
        int d = 1;
        iss >> d;
        const U64 n = perft(d);
        out("nodes " + std::to_string((unsigned long long)n) + "\n");
    }
    // stop, quit and ponderhit are handled by the input thread or ignored.
}

void readerLoop(std::mutex& qm, std::condition_variable& qcv, std::deque<std::string>& q) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "quit") {  // stop any search now; the main thread exits after earlier commands
            g_stopSignal = true;
            {
                std::lock_guard<std::mutex> lk(qm);
                q.push_back("quit");
            }
            qcv.notify_one();
            return;
        }
        if (cmd == "stop") {
            g_stopSignal = true;
            continue;
        }
        if (cmd == "isready" && g_searching.load()) {  // answered at once so it works during a search
            out("readyok\n");
            continue;
        }
        if (cmd == "go") {  // set here so a following stop cannot be lost
            g_stopSignal = false;
            g_searching = true;
        }
        {
            std::lock_guard<std::mutex> lk(qm);
            q.push_back(line);
        }
        qcv.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(qm);
        q.push_back("quit");
    }
    qcv.notify_one();
}

void initAll() {
    initAttacks();
    initSliders();
    initZobrist();
    initPSQ();
    initEvalMasks();
    for (int sq = 0; sq < 64; sq++) CASTLE_MASK[sq] = 15;
    CASTLE_MASK[4] &= ~(CR_WK | CR_WQ);
    CASTLE_MASK[0] &= ~CR_WQ;
    CASTLE_MASK[7] &= ~CR_WK;
    CASTLE_MASK[60] &= ~(CR_BK | CR_BQ);
    CASTLE_MASK[56] &= ~CR_BQ;
    CASTLE_MASK[63] &= ~CR_BK;
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++) g_lmr[d][m] = (int)(0.75 + std::log((double)d) * std::log((double)m) / 2.25);
    ttResize(256);
    setFen(START_FEN);
}

}  // namespace

int main(int argc, char** argv) {
    initAll();
    if (argc >= 4 && std::strcmp(argv[1], "--perft-suite") == 0)
        return runPerftSuite(argv[2], std::atoi(argv[3]));

    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::string> queue;
    std::thread reader(readerLoop, std::ref(qm), std::ref(qcv), std::ref(queue));
    reader.detach();

    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(qm);
            qcv.wait(lk, [&] { return !queue.empty(); });
            line = queue.front();
            queue.pop_front();
        }
        if (line == "quit") break;
        processCommand(line);
    }
    return 0;
}
