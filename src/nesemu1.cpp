#include "nesemu1.h"
#include <stdint.h>
#include <signal.h>
#include <assert.h>
#include <cmath>

#include <SDL.h>
#include <vector>

typedef uint_least32_t u32;
typedef uint_least16_t u16;
typedef uint_least8_t u8;
typedef int_least8_t s8;

// Bitfield
template<unsigned bitno, unsigned nbits = 1, typename T=u8>
struct RegBit {
    T data;
    enum {
        mask = (1u << nbits) - 1u
    };

    template<typename T2>
    RegBit &operator=(T2 val) {
        data = (data & ~(mask << bitno)) | ((nbits > 1 ? val & mask : !!val) << bitno);
        return *this;
    }

    explicit operator unsigned() const {
        return (data >> bitno) & mask;
    }

    RegBit &operator++() {
        return *this = *this + 1;
    }

    unsigned operator++(int) {
        unsigned r = *this;
        ++*this;
        return r;
    }
};


namespace CPU {
    u8 RAM[0x800];
    bool reset = true, nmi = false, nmi_edge_detected = false, intr = false;


    template<bool write>
    u8 MemAccess(u16 addr, u8 v = 0);

    u8 RB(u16 addr) {
        return MemAccess<0>(addr);
    }

    u8 WB(u16 addr, u8 v) {
        return MemAccess<1>(addr, v);
    }

    void tick() {
        //PPU clock: 3 times per cpu rate
        for (unsigned n = 0; n < 3; ++n) {
            PPU::tick();
        }

        //APU clock: 1 times per cpu rate
        for (unsigned n = 0; n < 1; ++n) {
            APU::tick();
        }

    }

    template<bool write>
    u8 MemAccess(u16 addr, u8 v) {
        tick();
        return 0;
    }


    // CPU registers
    u16 PC = 0xC000;
    u8 A = 0, X = 0, Y = 0, S = 0;

    // status flags
    union {
        u8 raw;
        RegBit<0> C; //carry
        RegBit<1> Z; //zero
        RegBit<2> I; //interrupt enable/disable
        RegBit<3> D; //decimal mode (unsupported on NES, but flag exist)
//        RegBit<4> C; (0x10, 0x20) don't exist
//        RegBit<5> C;
        RegBit<6> V; //overflow
        RegBit<7> N; //negative
    } P;

    u16 wrap(u16 oldaddr, u16 newaddr) {
        return (oldaddr & 0xFF00) + u8(newaddr);
    }

    void Misfire(u16 old, u16 addr) {
        u16 q = wrap(old, addr);
        if (q != addr) {
            RB(q);
        }
    }

    u8 Pop() {
        return RB(0x100 | u8(++S));
    }

    void Push(u8 v) {
        WB(0x100 | u8(S--), v);
    }

    template<u16 op>
    void Ins() {
        unsigned addr = 0, d = 0, t = 0xFF, c = 0, sb = 0, pbits = op < 0x100 ? 0x30 : 0x20;
        enum {
            o8 = op / 8, o8m = 1 << (op % 8)
        };

#define t(s, code) { enum { i=o8m & (s[o8]>90 ? (130+" (),-089<>?BCFGHJLSVWZ[^hlmnxy|}"[s[o8]-94]) : (s[o8]-" (("[s[o8]/39])) }; if(i) { code; } }

        /* Decode address operand */
        t("                                !", addr = 0xFFFA) // NMI vector location
        t("                                *", addr = 0xFFFC) // Reset vector location
        t("!                               ,", addr = 0xFFFE) // Interrupt vector location
        t("zy}z{y}zzy}zzy}zzy}zzy}zzy}zzy}z ", addr = RB(PC++))
        t("2 yy2 yy2 yy2 yy2 XX2 XX2 yy2 yy ", d = X) // register index
        t("  62  62  62  62  om  om  62  62 ", d = Y)
        t("2 y 2 y 2 y 2 y 2 y 2 y 2 y 2 y  ", addr=u8(addr+d); d=0; tick())              // add zeropage-index
        t(" y z!y z y z y z y z y z y z y z ", addr=u8(addr);   addr+=256*RB(PC++))       // absolute address
        t("3 6 2 6 2 6 286 2 6 2 6 2 6 2 6 /", addr=RB(c=addr); addr+=256*RB(wrap(c,c+1)))// indirect w/ page wrap
        t("  *Z  *Z  *Z  *Z      6z  *Z  *Z ", Misfire(addr, addr+d)) // abs. load: extra misread when cross-page
        t("  4k  4k  4k  4k  6z      4k  4k ", RB(wrap(addr, addr+d)))// abs. store: always issue a misread
        /* Load source operand */
        t("aa__ff__ab__,4  ____ -  ____     ", t &= A) // Many operations take A or X as operand. Some try in
        t("                knnn     4  99   ", t &= X) // error to take both; the outcome is an AND operation.
        t("                9989    99       ", t &= Y) // sty,dey,iny,tya,cpy
        t("                       4         ", t &= S) // tsx, las
        t("!!!!  !!  !!  !!  !   !!  !!  !!/", t &= P.raw|pbits; c = t)// php, flag test/set/clear, interrupts
        t("_^__dc___^__            ed__98   ", c = t; t = 0xFF)        // save as second operand
        t("vuwvzywvvuwvvuwv    zy|zzywvzywv ", t &= RB(addr+d)) // memory operand
        t(",2  ,2  ,2  ,2  -2  -2  -2  -2   ", t &= RB(PC++))   // immediate operand
        /* Operations that mogrify memory operands directly */
        t("    88                           ", P.V = t & 0x40; P.N = t & 0x80) // bit
        t("    nink    nnnk                 ", sb = P.C)       // rol,rla, ror,rra,arr
        t("nnnknnnk     0                   ", P.C = t & 0x80) // rol,rla, asl,slo,[arr,anc]
        t("        nnnknink                 ", P.C = t & 0x01) // lsr,sre, ror,rra,asr
        t("ninknink                         ", t = (t << 1) | (sb * 0x01))
        t("        nnnknnnk                 ", t = (t >> 1) | (sb * 0x80))
        t("                 !      kink     ", t = u8(t - 1))  // dec,dex,dey,dcp
        t("                         !  khnk ", t = u8(t + 1))  // inc,inx,iny,isb
        /* Store modified value (memory) */
        t("kgnkkgnkkgnkkgnkzy|J    kgnkkgnk ", WB(addr+d, t))
        t("                   q             ", WB(wrap(addr, addr+d), t &= ((addr+d) >> 8))) // [shx,shy,shs,sha?]
        /* Some operations used up one clock cycle that we did not account for yet */
        t("rpstljstqjstrjst - - - -kjstkjst/", tick()) // nop,flag ops,inc,dec,shifts,stack,transregister,interrupts
        /* Stack operations and unconditional jumps */
        t("     !  !    !                   ", tick(); t = Pop())                        // pla,plp,rti
        t("        !   !                    ", RB(PC++); PC = Pop(); PC |= (Pop() << 8)) // rti,rts
        t("            !                    ", RB(PC++))  // rts
        t("!   !                           /", d=PC+(op?-1:1); Push(d>>8); Push(d))      // jsr, interrupts
        t("!   !    8   8                  /", PC = addr) // jmp, jsr, interrupts
        t("!!       !                      /", Push(t))   // pha, php, interrupts
        /* Bitmasks */
        t("! !!  !!  !!  !!  !   !!  !!  !!/", t = 1)
        t("  !   !                   !!  !! ", t <<= 1)
        t("! !   !   !!  !!       !   !   !/", t <<= 2)
        t("  !   !   !   !        !         ", t <<= 4)
        t("   !       !           !   !____ ", t = u8(~t)) // sbc, isb,      clear flag
        t("`^__   !       !               !/", t = c | t)  // ora, slo,      set flag
        t("  !!dc`_  !!  !   !   !!  !!  !  ", t = c & t)  // and, bit, rla, clear/test flag
        t("        _^__                     ", t = c ^ t)  // eor, sre
        /* Conditional branches */
        t("      !       !       !       !  ", if(t)  { tick(); Misfire(PC, addr = s8(addr) + PC); PC=addr; })
        t("  !       !       !       !      ", if(!t) { tick(); Misfire(PC, addr = s8(addr) + PC); PC=addr; })
        /* Addition and subtraction */
        t("            _^__            ____ ", c = t; t += A + P.C; P.V = (c^t) & (A^t) & 0x80; P.C = t & 0x100)
        t("                        ed__98   ", t = c - t; P.C = ~t & 0x100) // cmp,cpx,cpy, dcp, sbx
        /* Store modified value (register) */
        t("aa__aa__aa__ab__ 4 !____    ____ ", A = t)
        t("                    nnnn 4   !   ", X = t) // ldx, dex, tax, inx, tsx,lax,las,sbx
        t("                 !  9988 !       ", Y = t) // ldy, dey, tay, iny
        t("                   4   0         ", S = t) // txs, las, shs
        t("!  ! ! !!  !   !       !   !   !/", P.raw = t & ~0x30) // plp, rti, flag set/clear
        /* Generic status flag updates */
        t("wwwvwwwvwwwvwxwv 5 !}}||{}wv{{wv ", P.N = t & 0x80)
        t("wwwv||wvwwwvwxwv 5 !}}||{}wv{{wv ", P.Z = u8(t) == 0)
        t("             0                   ", P.V = (((t >> 5)+1)&2))         // [arr]
        /* All implemented opcodes are cycle-accurate and memory-access-accurate.
         * [] means that this particular separate rule exists only to provide the indicated unofficial opcode(s).
         */
    }

    void Op() {
        //check the state of nmi flags
        bool nmi_now = nmi;
        unsigned op = RB(PC++);

        if (reset) {
            op = 0x101;
        } else if (nmi_now && !nmi_edge_detected) {
            op = 0x100;
            nmi_edge_detected = true;
        } else if (intr && !P.I) {
            op = 0x102;
        }
        if (!nmi_now) {
            nmi_edge_detected = false;
        }


        //define function pointers for each opcode (00..FF) and each interrupt (100,101,102)
#define c(n) Ins<0x##n>, Ins<0x##n+1>,
#define o(n) c(n)c(n+2)c(n+4)c(n+6)
        static void (*const i[0x108])() =
                {
                        o(00) o(08) o(10) o(18) o(20) o(28) o(30) o(38)
                        o(40) o(48) o(50) o(58) o(60) o(68) o(70) o(78)
                        o(80) o(88) o(90) o(98) o(A0) o(A8) o(B0) o(B8)
                        o(C0) o(C8) o(D0) o(D8) o(E0) o(E8) o(F0) o(F8) o(100)
                };
#undef o
#undef c
        i[op]();

        reset = false;
    }
}

