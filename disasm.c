#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <capstone.h>

#include "ndsdisasm.h"

extern void fatal_error(const char *fmt, ...);

#define min(x, y) ((x) < (y) ? (x) : (y))

uint32_t ROM_LOAD_ADDR;
#define UNKNOWN_SIZE (uint32_t)-1
int CsInitMode = CS_MODE_ARM;

enum BranchType
{
    BRANCH_TYPE_UNKNOWN,
    BRANCH_TYPE_B,
    BRANCH_TYPE_BL,
};

struct Label
{
    uint32_t addr;
    uint8_t type;
    uint8_t branchType;
    uint32_t size;
    bool processed;
    bool isFunc; // 100% sure it's a function, which cannot be changed to BRANCH_TYPE_B. 
    char *name;
};

struct Label *gLabels = NULL;
int gLabelsCount = 0;
static int sLabelBufferCount = 0;
static csh sCapstone;

const bool gOptionShowAddrComments = false;
const int gOptionDataColumnWidth = 16;

int disasm_add_label(uint32_t addr, uint8_t type, char *name)
{
    int i;
    if(addr < gRamStart) return 0;
    //printf("adding label 0x%08X\n", addr);
    // Search for label
    //assert(addr >= ROM_LOAD_ADDR && addr < ROM_LOAD_ADDR + gInputFileBufferSize);
    for (i = 0; i < gLabelsCount; i++)
    {
        if (gLabels[i].addr == addr)
        {
            gLabels[i].type = type;
            return i;
        }
    }

    i = gLabelsCount++;

    if (gLabelsCount > sLabelBufferCount) // need realloc
    {
        sLabelBufferCount = 2 * gLabelsCount;
        gLabels = realloc(gLabels, sLabelBufferCount * sizeof(*gLabels));

        if (gLabels == NULL)
            fatal_error("failed to alloc space for labels. ");
    }
    gLabels[i].addr = addr;
    gLabels[i].type = type;
    if (type == LABEL_ARM_CODE || type == LABEL_THUMB_CODE)
        gLabels[i].branchType = BRANCH_TYPE_BL;  // assume it's the start of a function
    else
        gLabels[i].branchType = BRANCH_TYPE_UNKNOWN;
    gLabels[i].size = UNKNOWN_SIZE;
    gLabels[i].processed = false;
    gLabels[i].name = name;
    gLabels[i].isFunc = false;

    if((addr - ROM_LOAD_ADDR) > gInputFileBufferSize)
    {
        gLabels[i].processed = true;
    }

    return i;
}

// Utility Functions

static struct Label *lookup_label(uint32_t addr)
{
    int i;

    for (i = 0; i < gLabelsCount; i++)
    {
        if (gLabels[i].addr == addr)
            return &gLabels[i];
    }
    return NULL;
}

static uint8_t byte_at(uint32_t addr)
{
    return gInputFileBuffer[addr - ROM_LOAD_ADDR];
}

static uint16_t hword_at(uint32_t addr)
{
    return (byte_at(addr + 0) << 0)
         | (byte_at(addr + 1) << 8);
}

static uint32_t word_at(uint32_t addr)
{
    return (byte_at(addr + 0) << 0)
         | (byte_at(addr + 1) << 8)
         | (byte_at(addr + 2) << 16)
         | (byte_at(addr + 3) << 24);
}

static int get_unprocessed_label_index(void)
{
    int i;

    for (i = 0; i < gLabelsCount; i++)
    {
        if (!gLabels[i].processed)
            return i;
    }
    return -1;
}

static bool is_branch(const struct cs_insn *insn)
{
    switch (insn->id)
    {
    case ARM_INS_B:
    case ARM_INS_BX:
    case ARM_INS_BL:
    case ARM_INS_BLX:
        return true;
    }
    return false;
}

static bool is_func_return(const struct cs_insn *insn)
{
    const struct cs_arm *arminsn = &insn->detail->arm;

    // 'bx' instruction
    if (insn->id == ARM_INS_BX)
        return arminsn->cc == ARM_CC_AL;
    // 'mov' with pc as the destination
    if (insn->id == ARM_INS_MOV
     && arminsn->operands[0].type == ARM_OP_REG
     && arminsn->operands[0].reg == ARM_REG_PC)
        return arminsn->cc == ARM_CC_AL;
    // 'pop' with pc in the register list
    if (insn->id == ARM_INS_POP)
    {
        int i;

        assert(arminsn->op_count > 0);
        for (i = 0; i < arminsn->op_count; i++)
        {
            if (arminsn->operands[i].type == ARM_OP_REG
             && arminsn->operands[i].reg == ARM_REG_PC)
                return arminsn->cc == ARM_CC_AL;
        }
    }
    return false;
}

static bool is_pool_load(const struct cs_insn *insn)
{
    const struct cs_arm *arminsn = &insn->detail->arm;

    if (insn->id == ARM_INS_LDR
     && arminsn->operands[0].type == ARM_OP_REG
     && arminsn->operands[1].type == ARM_OP_MEM
     && !arminsn->operands[1].subtracted
     && arminsn->operands[1].mem.base == ARM_REG_PC
     && arminsn->operands[1].mem.index == ARM_REG_INVALID)
        return true;
    else
        return false;
}

static uint32_t get_pool_load(const struct cs_insn *insn, uint32_t currAddr, int mode)
{
    assert(is_pool_load(insn));

    return (currAddr & ~3) + insn->detail->arm.operands[1].mem.disp + ((mode == LABEL_ARM_CODE) ? 8 : 4);
}

static uint32_t get_branch_target(const struct cs_insn *insn)
{
    assert(is_branch(insn));
    assert(insn->detail != NULL);

    return insn->detail->arm.operands[0].imm;
}

// Code Analysis

static int sJumpTableState = 0;

static void jump_table_state_machine_thumb(const struct cs_insn *insn, uint32_t addr)
{
    static uint32_t jumpTableBegin;
    // sometimes another instruction (like a mov) can interrupt
    static bool gracePeriod;

    switch (sJumpTableState)
    {
    case 0:
        // add rX, rX, rX
        gracePeriod = false;
        if (insn->id == ARM_INS_ADD && insn->detail->arm.operands[2].type == ARM_OP_REG && insn->detail->arm.operands[1].reg == insn->detail->arm.operands[2].reg)
            goto match;
        break;
    case 1:
        // add rX, pc
        if (insn->id == ARM_INS_ADD && insn->detail->arm.operands[1].type == ARM_OP_REG && insn->detail->arm.operands[1].reg == ARM_REG_PC)
            goto match;
        break;
    case 2:
        // ldrh rX, [rX, #imm]
        if (insn->id == ARM_INS_LDRH) {
            jumpTableBegin = insn->detail->arm.operands[1].mem.disp + addr + 2;
            goto match;
        }
        break;
    case 3:
        // lsls rX, 16
        if (insn->id == ARM_INS_LSL)
            goto match;
        break;
    case 4:
        // asrs rX, 16
        if (insn->id == ARM_INS_ASR)
            goto match;
        break;
    case 5:
        // add pc, rX
        if (insn->id == ARM_INS_ADD && insn->detail->arm.operands[0].reg == ARM_REG_PC)
        {
            goto match;
        }
        break;
    }
    // didn't match
    if (gracePeriod)
        sJumpTableState = 0;
    else
        gracePeriod = true;
    return;

    match:
    if (sJumpTableState == 5)  // all checks passed
    {
        uint32_t target;
        uint32_t firstTarget = -1u;

        // jump table is not in ROM, indicating it's from a library loaded into RAM
        assert(jumpTableBegin & ROM_LOAD_ADDR);
        disasm_add_label(jumpTableBegin, LABEL_JUMP_TABLE_THUMB, NULL);
        sJumpTableState = 0;
        // add code labels from jump table
        addr = jumpTableBegin;
        while (addr < firstTarget)
        {
            int label;

            target = hword_at(addr) + jumpTableBegin + 2;
            if (target - ROM_LOAD_ADDR >= 0x02000000)
                break;
            if (target & 1)
                break;
            if (target < firstTarget && target > jumpTableBegin)
                firstTarget = target;
            label = disasm_add_label(target, LABEL_THUMB_CODE, NULL);
            gLabels[label].branchType = BRANCH_TYPE_B;
            addr += 2;
        }

        return;
    }
    sJumpTableState++;
}

static void jump_table_state_machine(const struct cs_insn *insn, uint32_t addr, uint8_t type)
{
    static uint32_t jumpTableBegin;

    if (type == LABEL_THUMB_CODE) {
        jump_table_state_machine_thumb(insn, addr);
        return;
    }
    switch (sJumpTableState)
    {
    case 0:
        if (insn->id == ARM_INS_ADD
            && insn->detail->arm.operands[0].reg == ARM_REG_PC
            && insn->detail->arm.operands[2].type == ARM_OP_REG
            && insn->detail->arm.operands[2].shift.type == ARM_SFT_LSL
            && insn->detail->arm.operands[2].shift.value == 2)
            goto match;
        break;
    case 1:
        if ((insn->id == ARM_INS_B && insn->detail->arm.cc == ARM_CC_AL) || is_func_return(insn))
            goto match;
        break;
    }
    sJumpTableState = 0;
    return;
match:
    if (sJumpTableState == 1)
    {
        uint32_t target;
        uint32_t firstTarget = is_branch(insn) ? get_branch_target(insn) : -1u;
        int lbl;
        jumpTableBegin = addr + 4;
        sJumpTableState = 0;
        // add code labels from jump table
        addr = jumpTableBegin;
        int i = 0;
        lbl = disasm_add_label(addr, LABEL_JUMP_TABLE, NULL);
        while (addr < firstTarget)
        {
            int label;
            if (insn[i + 1].id == ARM_INS_B)
            {
                target = get_branch_target(&insn[i + 1]);
                if (target - ROM_LOAD_ADDR >= 0x02000000)
                {
                    break;
                }
                if (target < firstTarget && target > jumpTableBegin)
                {
                    firstTarget = target;
                }
                label = disasm_add_label(target, LABEL_ARM_CODE, NULL);
                gLabels[label].branchType = BRANCH_TYPE_B;
            }
            addr += 4;
            i++;
        }

        return;
    }
    sJumpTableState++;
}

static void renew_or_add_new_func_label(int type, uint32_t word)
{
    if (word & ROM_LOAD_ADDR)
    {
        struct Label *label_p = lookup_label(word & ~1);

        if (label_p != NULL)
        {
            // maybe it has been processed as a non-function label
            label_p->processed = false;
            label_p->branchType = BRANCH_TYPE_BL;
            label_p->isFunc = true;
        }
        else
        {
            // implicitly set to BRANCH_TYPE_BL
            gLabels[disasm_add_label(word & ~1, type, NULL)].isFunc = true;
        }
    }
}

static bool IsValidInstruction(cs_insn * insn, int type)
{
    if (cs_insn_group(sCapstone, insn, ARM_GRP_V5TE))
        return true;
    if (type == LABEL_ARM_CODE) {
        return cs_insn_group(sCapstone, insn, ARM_GRP_ARM);
    } else {
        return cs_insn_group(sCapstone, insn, ARM_GRP_THUMB);
    }
}

static void analyze(void)
{
    while (1)
    {
        int li;
        int i;
        uint32_t addr;
        int type;
        struct cs_insn *insn;
        const int dismAllocSize = 0x1000;
        int count;

        if ((li = get_unprocessed_label_index()) == -1)
            return;
        addr = gLabels[li].addr;
        type = gLabels[li].type;

        if (type == LABEL_ARM_CODE || type == LABEL_THUMB_CODE)
        {
            cs_option(sCapstone, CS_OPT_MODE, (type == LABEL_ARM_CODE) ? CS_MODE_ARM : CS_MODE_THUMB);
            sJumpTableState = 0;
            //fprintf(stderr, "analyzing label at 0x%08X\n", addr);
            do
            {
                uint32_t offset = addr - ROM_LOAD_ADDR;
                count = cs_disasm(sCapstone, gInputFileBuffer + offset, min(0x1000, gInputFileBufferSize - offset), addr, 0, &insn);
                for (i = 0; i < count; i++)
                {
                  no_inc:
                    if (!IsValidInstruction(&insn[i], type)) {
                        if (type == LABEL_THUMB_CODE)
                        {
                            int tmp_cnt;
                            cs_insn * tmp;
                            addr += 2;
                            if (insn[i].size == 2) continue;
                            tmp_cnt = cs_disasm(sCapstone, gInputFileBuffer + addr - ROM_LOAD_ADDR, 2, addr, 0, &tmp);
                            if (tmp_cnt == 1)
                            {
                                free(insn[i].detail);
                                insn[i] = *tmp;
                                free(tmp);
                            }
                            goto no_inc;
                        }
                        else
                        {
                            addr += 4;
                            continue;
                        }
                    };
                    jump_table_state_machine(&insn[i], addr, type);

                    // fprintf(stderr, "/*0x%08X*/ %s %s\n", addr, insn[i].mnemonic, insn[i].op_str);
                    if (is_branch(&insn[i]))
                    {
                        uint32_t target;
                        //uint32_t currAddr = addr;

                        addr += insn[i].size;

                        // For BX{COND}, only BXAL can be considered as end of function
                        if (is_func_return(&insn[i]))
                        {
                            struct Label *label_p;

                            // It's possible that handwritten code with different mode follows. 
                            // However, this only causes problem when the address following is
                            // incorrectly labeled as BRANCH_TYPE_B. 
                            label_p = lookup_label(addr);
                            if (label_p != NULL
                             && (label_p->type == LABEL_THUMB_CODE || label_p->type == LABEL_ARM_CODE)
                             && label_p->type != type
                             && label_p->branchType == BRANCH_TYPE_B)
                            {
                                label_p->branchType = BRANCH_TYPE_BL;
                                label_p->isFunc = true;
                            }
                            break;
                        }

                        if (insn[i].id == ARM_INS_BX) // BX{COND} when COND != AL
                            continue;

                        target = get_branch_target(&insn[i]);
                        assert(target != 0);

                        // I don't remember why I needed this condition
                        //if (!(target >= gLabels[li].addr && target <= currAddr))
                        if (1)
                        {
                            int lbl = disasm_add_label(target, type, NULL);

                            if (!gLabels[lbl].isFunc) // do nothing if it's 100% a func (from func ptr, or instant mode exchange)
                            {
                                if (insn[i].id == ARM_INS_BL || insn[i].id == ARM_INS_BLX)
                                {
                                    const struct Label *next;

                                    uint8_t newtype = type;
                                    if (insn[i].id == ARM_INS_BLX)
                                    {
                                        newtype = type == LABEL_THUMB_CODE ? LABEL_ARM_CODE : LABEL_THUMB_CODE;
                                        if (insn[i].detail->arm.operands[0].type == ARM_OP_REG)
                                            goto blx_rX;
                                    }
                                    lbl = disasm_add_label(target, newtype, NULL);
                                    if (gLabels[lbl].branchType != BRANCH_TYPE_B)
                                        gLabels[lbl].branchType = BRANCH_TYPE_BL;
                                    if (insn[i].id != ARM_INS_BLX)
                                    {
                                        // if the address right after is a pool, then we know
                                        // for sure that this is a far jump and not a function call
                                        if (((next = lookup_label(addr)) != NULL && next->type == LABEL_POOL)
                                            // if the 2 bytes following are zero, assume it's padding
                                            || (type == LABEL_THUMB_CODE && ((addr & 3) != 0) && hword_at(addr) == 0))
                                        {
                                            gLabels[lbl].branchType = BRANCH_TYPE_B;
                                            break;
                                        }
                                    }
                                }
                                else
                                {
                                    // the label might be given a name in .cfg file, but it's actually not a function
                                    if (gLabels[lbl].name != NULL)
                                        free(gLabels[lbl].name);
                                    gLabels[lbl].name = NULL;
                                    gLabels[lbl].branchType = BRANCH_TYPE_B;
                                }
                            }
                        }
                        blx_rX:
                        // unconditional jump and not a function call
                        if (insn[i].detail->arm.cc == ARM_CC_AL && insn[i].id != ARM_INS_BL && insn[i].id != ARM_INS_BLX)
                            break;
                    }
                    else
                    {
                        uint32_t poolAddr;
                        uint32_t word;

                        addr += insn[i].size;

                        if (is_func_return(&insn[i]))
                        {
                            struct Label *label_p;

                            // It's possible that handwritten code with different mode follows. 
                            // However, this only causes problem when the address following is
                            // incorrectly labeled as BRANCH_TYPE_B. 
                            label_p = lookup_label(addr);
                            if (label_p != NULL
                             && (label_p->type == LABEL_THUMB_CODE || label_p->type == LABEL_ARM_CODE)
                             && label_p->type != type
                             && label_p->branchType == BRANCH_TYPE_B)
                            {
                                label_p->branchType = BRANCH_TYPE_BL;
                                label_p->isFunc = true;
                            }
                            break;
                        }

                        assert(insn[i].detail != NULL);

                        // looks like that this check can only detect thumb mode
                        // anyway I still put the arm mode things here for a potential future fix
                        if (insn[i].id == ARM_INS_ADR)
                        {
                            word = insn[i].detail->arm.operands[1].imm + (addr - insn[i].size)
                                 + (type == LABEL_THUMB_CODE ? 4 : 8);
                            if (type == LABEL_THUMB_CODE)
                                word &= ~3;
                            goto check_handwritten_indirect_jump;
                        }

                        // fix above check for arm mode
                        if (type == LABEL_ARM_CODE
                         && insn[i].id == ARM_INS_ADD
                         && insn[i].detail->arm.operands[0].type == ARM_OP_REG
                         && insn[i].detail->arm.operands[1].type == ARM_OP_REG
                         && insn[i].detail->arm.operands[1].reg == ARM_REG_PC
                         && insn[i].detail->arm.operands[2].type == ARM_OP_IMM)
                        {
                            word = insn[i].detail->arm.operands[2].imm + (addr - insn[i].size) + 8;
                            goto check_handwritten_indirect_jump;
                        }

                        if (is_pool_load(&insn[i]))
                        {
                            poolAddr = get_pool_load(&insn[i], addr - insn[i].size, type);
                            assert(poolAddr != 0);
                            assert((poolAddr & 3) == 0);
                            disasm_add_label(poolAddr, LABEL_POOL, NULL);
                            word = word_at(poolAddr);

                        check_handwritten_indirect_jump:
                            if (i < count - 1) // is not last insn in the chunk
                            {
                                // check if it's followed with bx RX or mov PC, RX (conditional won't hurt)
                                if (insn[i + 1].id == ARM_INS_BX)
                                {
                                    if (insn[i + 1].detail->arm.operands[0].type == ARM_OP_REG
                                     && insn[i].detail->arm.operands[0].reg == insn[i + 1].detail->arm.operands[0].reg)
                                        renew_or_add_new_func_label(word & 1 ? LABEL_THUMB_CODE : LABEL_ARM_CODE, word);
                                }
                                else if (insn[i + 1].id == ARM_INS_MOV
                                      && insn[i + 1].detail->arm.operands[0].type == ARM_OP_REG
                                      && insn[i + 1].detail->arm.operands[0].reg == ARM_REG_PC
                                      && insn[i + 1].detail->arm.operands[1].type == ARM_OP_REG
                                      && insn[i].detail->arm.operands[0].reg == insn[i + 1].detail->arm.operands[1].reg)
                                {
                                    renew_or_add_new_func_label(type, word);
                                }
                            }
                        }
                    }
                }
                cs_free(insn, count);
            } while (count == dismAllocSize);
            gLabels[li].processed = true;
            gLabels[li].size = addr - gLabels[li].addr;
        }
        gLabels[li].processed = true;
    }
}

// Disassembly Output

static void print_gap(uint32_t addr, uint32_t nextaddr)
{
    if (addr == nextaddr)
        return;

    assert(addr < nextaddr);

    if ((addr & 3) == 2) {
        uint16_t next_short = hword_at(addr);
        if (next_short == 0) {
            fputs("\t.align 2, 0\n", stdout);
            addr += 2;
        } else if (next_short == 0x46C0) {
            fputs("\tnop\n", stdout);
            addr += 2;
        }
        if (addr == nextaddr) {
            return;
        }
    }

    printf("_%08X:\n", addr);
    if (addr % gOptionDataColumnWidth != 0)
        fputs("\t.byte", stdout);
    while (addr < nextaddr)
    {
        if (addr % gOptionDataColumnWidth == 0)
            fputs("\t.byte", stdout);
        if (addr % gOptionDataColumnWidth == (unsigned int)(gOptionDataColumnWidth - 1)
         || addr == nextaddr - 1)
            printf(" 0x%02X\n", byte_at(addr));
        else
            printf(" 0x%02X,", byte_at(addr));
        addr++;
    }
}

static void do_print_insn(const char * fmt, int caseNum, ...)
{
    va_list va_args;
    va_start(va_args, caseNum);
    char * true_fmt = strdup(fmt);
    true_fmt[strlen(true_fmt) - 1] = 0;
    vprintf(true_fmt, va_args);
    if (caseNum >= 0)
        printf(" @ case %d\n", caseNum);
    else
        putchar('\n');
    free(true_fmt);
    va_end(va_args);
}

static void print_insn(const cs_insn *insn, uint32_t addr, int mode, int caseNum)
{
    struct Label DummyLabel;
    if (gOptionShowAddrComments)
    {
        do_print_insn("\t/*0x%08X*/ %s %s\n", caseNum, addr, insn->mnemonic, insn->op_str);
    }
    else
    {
        if (is_branch(insn) && insn->detail->arm.operands[0].type != ARM_OP_REG)
        {
            uint32_t target = get_branch_target(insn);
            struct Label *label = lookup_label(target);

            if (isFullRom)
                assert(label != NULL);  // We should have found this label in the analysis phase
            else if (label == NULL) {
                DummyLabel.addr = target;
                DummyLabel.name = NULL;
                DummyLabel.branchType = BRANCH_TYPE_BL;
                label = &DummyLabel;
            }
            if (label->name != NULL)
                do_print_insn("\t%s %s\n", caseNum, insn->mnemonic, label-> name);
            else
                do_print_insn("\t%s %s_%08X\n", caseNum, insn->mnemonic, label->branchType == BRANCH_TYPE_BL ? "FUN" : "", target);
        }
        else if (is_pool_load(insn))
        {
            uint32_t word = get_pool_load(insn, addr, mode);
            uint32_t value = word_at(word);
            const struct Label *label_p;

            if (value & 3 && value & ROM_LOAD_ADDR) // possibly thumb function
            {
                if (label_p = lookup_label(value & ~1), label_p != NULL)
                {
                    if (label_p->branchType == BRANCH_TYPE_BL && label_p->type == LABEL_THUMB_CODE)
                    {
                        if (label_p->name != NULL)
                            do_print_insn("\t%s %s, _%08X @ =%s\n", caseNum, insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, label_p->name);
                        else
                            do_print_insn("\t%s %s, _%08X @ =FUN_%08X\n", caseNum, insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, value & ~1);
                        return;
                    }
                }
            }
            label_p = lookup_label(value);
            if (label_p != NULL)
            {
                if (label_p->type != LABEL_THUMB_CODE)
                {
                    if (label_p->name != NULL)
                        do_print_insn("\t%s %s, _%08X @ =%s\n", caseNum, insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, label_p->name);
                    else if (label_p->branchType == BRANCH_TYPE_BL)
                        do_print_insn("\t%s %s, _%08X @ =FUN_%08X\n", caseNum, insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, value);
                    else // normal label
                        do_print_insn("\t%s %s, _%08X @ =_%08X\n", caseNum,
                          insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, value);
                    return;
                }
            }
            do_print_insn("\t%s %s, _%08X @ =0x%08X\n", caseNum, insn->mnemonic, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), word, value);
        }
        else
        {
            // fix "add rX, sp, rX"
            if (insn->id == ARM_INS_ADD
             && insn->detail->arm.operands[0].type == ARM_OP_REG
             && insn->detail->arm.operands[1].type == ARM_OP_REG
             && insn->detail->arm.operands[1].reg == ARM_REG_SP
             && insn->detail->arm.operands[2].type == ARM_OP_REG)
            {
                do_print_insn("\t%s %s, %s\n", caseNum,
                  insn->mnemonic,
                  cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg),
                  cs_reg_name(sCapstone, insn->detail->arm.operands[1].reg));
            }
            // fix thumb adr
            else if (insn->id == ARM_INS_ADR && mode == LABEL_THUMB_CODE)
            {
                uint32_t word = (insn->detail->arm.operands[1].imm + addr + 4) & ~3;
                const struct Label *label_p = lookup_label(word);

                if (label_p != NULL)
                {
                    if (label_p->type != LABEL_THUMB_CODE)
                    {
                        if (label_p->name != NULL)
                            do_print_insn("\tadd %s, pc, #0x%X @ =%s\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[1].imm, label_p->name);
                        else if (label_p->branchType == BRANCH_TYPE_BL)
                            do_print_insn("\tadd %s, pc, #0x%X @ =FUN_%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[1].imm, word);
                        else
                            do_print_insn("\tadd %s, pc, #0x%X @ =_%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[1].imm, word);
                        return;
                    }
                }
                do_print_insn("\tadd %s, pc, #0x%X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[1].imm);
            }
            // arm adr
            else if (mode == LABEL_ARM_CODE
                  && insn->id == ARM_INS_ADD
                  && insn->detail->arm.operands[0].type == ARM_OP_REG
                  && insn->detail->arm.operands[1].type == ARM_OP_REG
                  && insn->detail->arm.operands[1].reg == ARM_REG_PC
                  && insn->detail->arm.operands[2].type == ARM_OP_IMM)
            {
                uint32_t word = insn->detail->arm.operands[2].imm + addr + 8;
                const struct Label *label_p;

                if (word & 3 && word & ROM_LOAD_ADDR) // possibly thumb function
                {
                    if (label_p = lookup_label(word & ~1), label_p != NULL)
                    {
                        if (label_p->branchType == BRANCH_TYPE_BL && label_p->type == LABEL_THUMB_CODE)
                        {
                            if (label_p->name != NULL)
                                do_print_insn("\tadd %s, pc, #0x%X @ =%s\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, label_p->name);
                            else
                                do_print_insn("\tadd %s, pc, #0x%X @ =FUN_%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, word & ~1);
                            return;
                        }
                    }
                }
                label_p = lookup_label(word);
                if (label_p != NULL)
                {
                    if (label_p->type != LABEL_THUMB_CODE)
                    {
                        if (label_p->name != NULL)
                            do_print_insn("\tadd %s, pc, #0x%X @ =%s\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, label_p->name);
                        else if (label_p->branchType == BRANCH_TYPE_BL)
                            do_print_insn("\tadd %s, pc, #0x%X @ =FUN_%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, word);
                        else
                            do_print_insn("\tadd %s, pc, #0x%X @ =_%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, word);
                        return;
                    }
                }
                do_print_insn("\tadd %s, pc, #0x%X @ =0x%08X\n", caseNum, cs_reg_name(sCapstone, insn->detail->arm.operands[0].reg), insn->detail->arm.operands[2].imm, word);
            }
            else
                do_print_insn("\t%s %s\n", caseNum, insn->mnemonic, insn->op_str);
        }
    }
}

static int qsort_label_compare(const void *a, const void *b)
{
    return ((struct Label *)a)->addr - ((struct Label *)b)->addr;
}

static void print_disassembly(void)
{
    //uint32_t addr = ROM_LOAD_ADDR;
    int i = 0;

    qsort(gLabels, gLabelsCount, sizeof(*gLabels), qsort_label_compare);
    uint32_t addr = gLabels[0].addr;

    for (i = 0; i < gLabelsCount - 1; i++)
        assert(gLabels[i].addr < gLabels[i + 1].addr);
    for (i = 0; i < gLabelsCount; i++)
    {
        if (gLabels[i].type == LABEL_ARM_CODE || gLabels[i].type == LABEL_THUMB_CODE)
            assert(gLabels[i].processed);
    }
    // check mode exchange right after func return
    for (i = 1; i < gLabelsCount; i++)
        if ((gLabels[i - 1].type == LABEL_ARM_CODE && gLabels[i].type == LABEL_THUMB_CODE)
         || (gLabels[i - 1].type == LABEL_THUMB_CODE && gLabels[i].type == LABEL_ARM_CODE))
            gLabels[i].branchType = BRANCH_TYPE_BL;

    i = 0;
    while (addr < ROM_LOAD_ADDR + gInputFileBufferSize)
    {
        uint32_t nextAddr;

        // TODO: compute actual size during analysis phase
        if (i + 1 < gLabelsCount)
        {
            if (gLabels[i].size == UNKNOWN_SIZE
             || gLabels[i].addr + gLabels[i].size > gLabels[i + 1].addr)
                gLabels[i].size = gLabels[i + 1].addr - gLabels[i].addr;
        }

        switch (gLabels[i].type)
        {
        case LABEL_ARM_CODE:
        case LABEL_THUMB_CODE:
            {
                struct cs_insn *insn;
                int count;
                int j;
                int mode = (gLabels[i].type == LABEL_ARM_CODE) ? CS_MODE_ARM : CS_MODE_THUMB;

                // This is a function. Use the 'sub_XXXXXXXX' label
                if (gLabels[i].branchType == BRANCH_TYPE_BL)
                {
                    unsigned int unalignedMask = (mode == CS_MODE_ARM) ? 3 : 1;

                    if (addr & unalignedMask)
                    {
                        fprintf(stderr, "error: function at 0x%08X is not aligned\n", addr);
                        return;
                    }
                    if (gLabels[i].name != NULL)
                    {
                        printf("\n\t%s %s\n",
                          (gLabels[i].type == LABEL_ARM_CODE) ? "arm_func_start" : (addr & 2 ? "non_word_aligned_thumb_func_start" : "thumb_func_start"),
                          gLabels[i].name);
                        printf("%s: @ 0x%08X\n", gLabels[i].name, addr);
                    }
                    else
                    {
                        printf("\n\t%s FUN_%08X\n",
                          (gLabels[i].type == LABEL_ARM_CODE) ? "arm_func_start" : (addr & 2 ? "non_word_aligned_thumb_func_start" : "thumb_func_start"),
                          addr);
                        printf("FUN_%08X: @ 0x%08X\n", addr, addr);
                    }
                }
                // Just a normal code label. Use the '_XXXXXXXX' label
                else
                {
                    if (gLabels[i].name != NULL)
                        printf("%s:\n", gLabels[i].name);
                    else
                        printf("_%08X:\n", addr);
                }

                assert(gLabels[i].size != UNKNOWN_SIZE);
                cs_option(sCapstone, CS_OPT_MODE, mode);
                count = cs_disasm(sCapstone, gInputFileBuffer + addr - ROM_LOAD_ADDR, gLabels[i].size, addr, 0, &insn);
                for (j = 0; j < count; j++)
                {
                  no_inc:
                    if (!IsValidInstruction(&insn[j], gLabels[i].type)) {
                        if (gLabels[i].type == LABEL_THUMB_CODE)
                        {
                            int tmp_cnt;
                            cs_insn * tmp;
                            printf("\t.hword 0x%04X\n", hword_at(addr));
                            addr += 2;
                            if (insn[j].size == 2) continue;
                            tmp_cnt = cs_disasm(sCapstone, gInputFileBuffer + addr - ROM_LOAD_ADDR, 2, addr, 0, &tmp);
                            if (tmp_cnt == 1)
                            {
                                free(insn[j].detail);
                                insn[j] = *tmp;
                                free(tmp);
                            }
                            goto no_inc;
                        }
                        else
                        {
                            printf("\t.word 0x%08X\n", word_at(addr));
                            addr += 4;
                            continue;
                        }
                    }
                    print_insn(&insn[j], addr, gLabels[i].type, -1);
                    addr += insn[j].size;
                }
                cs_free(insn, count);

                // align pool if it comes next
                if (i + 1 < gLabelsCount && gLabels[i + 1].type == LABEL_POOL)
                {
                    const uint8_t zeros[3] = {0};
                    int diff = gLabels[i + 1].addr - addr;
                    if (diff == 0
                     || (diff > 0 && diff < 4 && memcmp(gInputFileBuffer + addr - ROM_LOAD_ADDR, zeros, diff) == 0))
                    {
                        puts("\t.align 2, 0");
                        addr += diff;
                    }
                }
            }
            break;
        case LABEL_POOL:
            {
                uint32_t value = word_at(addr);
                const struct Label *label_p;

                if (value & 3 && value & ROM_LOAD_ADDR) // possibly thumb function
                {
                    if (label_p = lookup_label(value & ~1), label_p != NULL)
                    {
                        if (label_p->branchType == BRANCH_TYPE_BL && label_p->type == LABEL_THUMB_CODE)
                        {
                            if (label_p->name != NULL)
                                printf("_%08X: .4byte %s\n", addr, label_p->name);
                            else
                                printf("_%08X: .4byte FUN_%08X\n", addr, value & ~1);
                            addr += 4;
                            break;
                        }
                    }
                }
                label_p = lookup_label(value);
                if (label_p != NULL)
                {
                    if (label_p->type != LABEL_THUMB_CODE)
                    {
                        if (label_p->name != NULL)
                            printf("_%08X: .4byte %s\n", addr, label_p->name);
                        else if (label_p->branchType == BRANCH_TYPE_BL)
                            printf("_%08X: .4byte FUN_%08X\n", addr, value);
                        else // normal label
                            printf("_%08X: .4byte _%08X\n", addr, value);
                        addr += 4;
                        break;
                    }
                }
                printf("_%08X: .4byte 0x%08X\n", addr, value);
                addr += 4;
            }
            break;
        case LABEL_JUMP_TABLE_THUMB:
        {
            uint32_t start = addr;
            uint32_t end = addr + gLabels[i].size;
            int caseNum = 0;

            printf("_%08X: @ jump table\n", addr);
            while (addr < end)
            {
                uint16_t offset = hword_at(addr);
                uint32_t word = start + offset + 2;

                printf("\t.2byte _%08X - _%08X - 2 @ case %i\n", word, start, caseNum);
                caseNum++;
                addr += 2;
            }
        }
            break;
        case LABEL_JUMP_TABLE:
            {
                struct cs_insn * insn;
                cs_option(sCapstone, CS_OPT_MODE, CS_MODE_ARM);
                int count = cs_disasm(sCapstone, gInputFileBuffer + addr - ROM_LOAD_ADDR, gLabels[i].size, addr, 0, &insn);
                int caseNum = 0;

                printf("_%08X: @ jump table\n", addr);
                for (caseNum = 0; caseNum < count; caseNum++)
                {
                    print_insn(&insn[caseNum], addr, LABEL_ARM_CODE, caseNum);
                    addr += 4;
                }
                cs_free(insn, count);
            }
            break;
        }

        i++;
        if (i >= gLabelsCount)
            break;
        nextAddr = gLabels[i].addr;
        assert(addr <= nextAddr);

        if (nextAddr <= ROM_LOAD_ADDR + gInputFileBufferSize) // prevent out-of-bound read
            print_gap(addr, nextAddr);
        addr = nextAddr;
    }
}

void disasm_disassemble(void)
{
    // initialize capstone
    if (cs_open(CS_ARCH_ARM, CsInitMode, &sCapstone) != CS_ERR_OK)
    {
        puts("cs_open failed");
        return;
    }
    cs_option(sCapstone, CS_OPT_DETAIL, CS_OPT_ON);

    // entry point
    disasm_add_label(ROM_LOAD_ADDR, CsInitMode == CS_MODE_ARM ? LABEL_ARM_CODE : LABEL_THUMB_CODE, NULL);

    // rom header
    //disasm_add_label(ROM_LOAD_ADDR + 4, LABEL_DATA, NULL);

    analyze();
    print_disassembly();
    free(gLabels);
}
